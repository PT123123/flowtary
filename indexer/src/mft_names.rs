//! Pure, bounded FILE-record decoding. An extension fragment is not a complete
//! file: the future bulk reader must merge by owner and validate link counts.
use anyhow::{Result,ensure};
use goz_core::{layout::LayoutName,types::Frn};
use mft::{MftEntry,attribute::{MftAttributeContent,MftAttributeType,header::ResidentialHeader,x30::FileNamespace}};
#[cfg(test)]
use serde::{Deserialize, Serialize};
#[cfg(test)]
use std::collections::HashMap;
use std::collections::HashSet;

#[cfg(test)]
#[derive(Default)]
pub struct LinkAudit {
    owners:HashMap<Frn,(Option<u16>,u32,bool)>,
    expected_extensions:HashMap<Frn,HashSet<Frn>>,
    seen_extensions:HashSet<Frn>,
}
/// Serializable, versioned state required to continue hard-link validation
/// after a bounded enumeration segment. FRNs are stored as raw u64s so this
/// stays independent of serde support in the vendored upstream identifier.
#[cfg(test)]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct LinkAuditCheckpoint {
    pub schema:u32,
    pub owners:Vec<(u64,Option<u16>,u32,bool)>,
    pub expected_extensions:Vec<(u64,Vec<u64>)>,
    pub seen_extensions:Vec<u64>,
}
#[cfg(test)]
impl LinkAudit {
    pub fn checkpoint(&self)->LinkAuditCheckpoint {
        let mut owners:Vec<_>=self.owners.iter().map(|(frn,(links,count,base))|(frn.0,*links,*count,*base)).collect();
        owners.sort_unstable_by_key(|x|x.0);
        let mut expected_extensions:Vec<_>=self.expected_extensions.iter().map(|(owner,extensions)|{
            let mut extensions:Vec<_>=extensions.iter().map(|frn|frn.0).collect();extensions.sort_unstable();(owner.0,extensions)
        }).collect();
        expected_extensions.sort_unstable_by_key(|x|x.0);
        let mut seen_extensions:Vec<_>=self.seen_extensions.iter().map(|frn|frn.0).collect();seen_extensions.sort_unstable();
        LinkAuditCheckpoint {schema:1,owners,expected_extensions,seen_extensions}
    }
    pub fn from_checkpoint(saved:LinkAuditCheckpoint)->Result<Self> {
        ensure!(saved.schema==1,"unsupported MFT link-audit checkpoint schema");
        ensure!(saved.owners.len()<=1_000_000 && saved.expected_extensions.len()<=1_000_000 &&
            saved.seen_extensions.len()<=1_000_000,"MFT link-audit checkpoint exceeds record budget");
        let mut audit=Self::default();
        for (raw,links,count,base) in saved.owners {
            let owner=Frn(raw);
            ensure!(count<=u32::from(links.unwrap_or(u16::MAX)),"invalid MFT checkpoint link count");
            ensure!(!base || links.is_some(),"MFT checkpoint base record has no hard-link count");
            ensure!(audit.owners.insert(owner,(links,count,base)).is_none(),"duplicate owner in MFT link-audit checkpoint");
        }
        for (raw,extensions) in saved.expected_extensions {
            ensure!(extensions.len()<=1024,"MFT checkpoint extension-record budget exceeded");
            ensure!(audit.owners.contains_key(&Frn(raw)),"MFT checkpoint extensions have no owner");
            let set:HashSet<_>=extensions.into_iter().map(Frn).collect();
            ensure!(set.len()<=1024,"duplicate extension in MFT link-audit checkpoint");
            ensure!(audit.expected_extensions.insert(Frn(raw),set).is_none(),"duplicate expected-extension owner in MFT checkpoint");
        }
        for (owner,(_,_,base)) in &audit.owners {
            if *base { ensure!(audit.expected_extensions.contains_key(owner),"MFT checkpoint omits a base record's extension set"); }
        }
        for raw in saved.seen_extensions {
            ensure!(audit.seen_extensions.insert(Frn(raw)),"duplicate seen extension in MFT checkpoint");
        }
        Ok(audit)
    }
    pub fn observe(&mut self,fragment:&Fragment)->Result<()> {
        if !self.owners.contains_key(&fragment.owner) {ensure!(self.owners.len()<1_000_000,"MFT link-audit owner budget exceeded");}
        let state=self.owners.entry(fragment.owner).or_insert((None,0,false));
        if fragment.is_base {
            ensure!(!state.2,"duplicate base FILE record in MFT enumeration");
            state.0=fragment.base_link_count;state.2=true;
            ensure!(fragment.extension_records.len()<=1024,"MFT extension-record budget exceeded");
            let extensions:HashSet<_>=fragment.extension_records.iter().copied().collect();
            ensure!(extensions.len()==fragment.extension_records.len(),"duplicate extension FRN in base FILE record");
            self.expected_extensions.insert(fragment.owner,extensions);
        } else {
            ensure!(self.seen_extensions.insert(fragment.record),"duplicate MFT extension FILE record");
        }
        state.1=state.1.checked_add(fragment.file_name_attributes as u32).ok_or_else(||anyhow::anyhow!("MFT FILE_NAME attribute count overflow"))?;
        if let Some(expected)=state.0 {ensure!(state.1<=u32::from(expected),"MFT FILE_NAME attributes exceed base hard-link count");}
        Ok(())
    }
    pub fn finish(&self)->Result<()> {
        ensure!(!self.owners.is_empty(),"MFT enumeration produced no allocated FILE_NAME records");
        for (owner,(expected,observed,base_seen)) in &self.owners {
            ensure!(*base_seen,"MFT extension record {owner:?} has no enumerated base record");
            let expected=expected.ok_or_else(||anyhow::anyhow!("MFT base record {owner:?} has no link-count field"))?;
            ensure!(*observed==u32::from(expected),"MFT FILE_NAME attribute coverage incomplete for {owner:?}: observed {observed}, base link count {expected}");
        }
        for (owner,expected) in &self.expected_extensions {
            for extension in expected {ensure!(self.seen_extensions.contains(extension),
                "MFT FILE_NAME extension {extension:?} referenced by {owner:?} was not read");}
        }
        Ok(())
    }
}

pub struct Fragment {
    pub record:Frn,
    pub owner:Frn,
    pub is_base:bool,
    pub directory:bool,
    pub base_link_count:Option<u16>,
    pub names:Vec<LayoutName>,
    pub kernel_restored_fixups:bool,
    pub file_name_attributes:usize,
    pub dos_only_attributes:usize,
    pub has_attribute_list:bool,
    pub nonresident_attribute_list:bool,
    pub extension_records:Vec<Frn>,
}
fn reference(entry:u64,sequence:u16)->Result<Frn>{
    ensure!(entry<(1u64<<48),"MFT record number exceeds NTFS 64-bit reference capacity");
    Ok(Frn(entry|((sequence as u64)<<48)))
}
pub fn decode(raw:&[u8],record_number:u64)->Result<Option<Fragment>> {
    decode_inner(raw,record_number,false)
}
/// NTFS may return an already-restored cached record through GET_FILE_RECORD.
/// Accept that representation only when *every* sector tail matches its USA
/// replacement. Raw-volume data must use decode(), which always requires USA tags.
pub fn decode_fsctl(raw:&[u8],record_number:u64)->Result<Option<Fragment>> {
    decode_inner(raw,record_number,true)
}
fn restored_tails_match(raw:&[u8])->bool {
    let offset=u16::from_le_bytes([raw[4],raw[5]]) as usize;
    let count=u16::from_le_bytes([raw[6],raw[7]]) as usize;
    if count<2 || offset<8 || offset+count*2>raw.len() || raw.len()%(count-1)!=0{return false}
    let sector=raw.len()/(count-1);
    if !(512..=4096).contains(&sector) || !sector.is_power_of_two(){return false}
    (1..count).all(|i|raw[i*sector-2..i*sector]==raw[offset+i*2..offset+i*2+2])
}
fn fixup_diagnostic(raw:&[u8])->String {
    if raw.len()<8 {return format!("record is only {} bytes",raw.len())}
    let offset=u16::from_le_bytes([raw[4],raw[5]]) as usize;
    let count=u16::from_le_bytes([raw[6],raw[7]]) as usize;
    if count<2 || offset+count*2>raw.len(){return format!("record={} USA offset/count out of bounds",raw.len())}
    let tag=&raw[offset..offset+2];
    let sector_count=count-1;
    let sector=raw.len()/sector_count;
    let mut tails=Vec::with_capacity(sector_count);
    for i in 1..=sector_count {
        let tail=&raw[i*sector-2..i*sector];
        let replacement=&raw[offset+i*2..offset+i*2+2];
        tails.push(format!("{:02x}{:02x}:{:02x}{:02x}",tail[0],tail[1],replacement[0],replacement[1]));
    }
    format!("record_bytes={} USA_offset={offset} USA_count={count} tag={} tail:replacement={}",
        raw.len(),format!("{:02x}{:02x}",tag[0],tag[1]),tails.join(","))
}
fn decode_inner(raw:&[u8],record_number:u64,allow_restored:bool)->Result<Option<Fragment>> {
    ensure!((512..=65536).contains(&raw.len()) && raw.len().is_power_of_two(),"invalid FILE record byte length");
    ensure!(record_number<(1u64<<48),"FILE record number exceeds 48 bits");
    if raw.iter().all(|b|*b==0){return Ok(None)}
    let mut entry=MftEntry::from_buffer(raw.to_vec(),record_number)?;
    ensure!(entry.header.is_valid(),"invalid/BAAD FILE record signature");
    let kernel_restored=entry.valid_fixup!=Some(true) && allow_restored && restored_tails_match(raw);
    if kernel_restored {entry=MftEntry::from_buffer_skip_fixup(raw.to_vec(),record_number)?;}
    ensure!(kernel_restored || entry.valid_fixup==Some(true),"FILE record update-sequence fixup mismatch for FRN {record_number}: {}",fixup_diagnostic(raw));
    if !entry.is_allocated(){return Ok(None)}
    ensure!(entry.header.total_entry_size as usize==raw.len() && entry.header.used_entry_size as usize<=raw.len(),"FILE record size fields exceed buffer");
    let record=reference(record_number,entry.header.sequence)?;
    let is_base=entry.header.is_base_record();
    let owner=if is_base{record}else{reference(entry.header.base_reference.entry,entry.header.base_reference.sequence)?};
    ensure!(is_base || owner!=record,"extension FILE record refers to itself");
    ensure!(entry.header.hard_link_count<=1024,"MFT hard-link count exceeds budget");
    let mut has_attribute_list=false;let mut nonresident_attribute_list=false;let mut extension_records=HashSet::new();
    for attribute in entry.iter_attribute_records_matching(Some(vec![MftAttributeType::AttributeList])) {
        let (header,record)=attribute?;has_attribute_list=true;
        if matches!(&header.residential_header,ResidentialHeader::NonResident(_)) {
            nonresident_attribute_list=true;continue;
        }
        let content=MftAttributeContent::from_record(record,&header)?;
        if let Some(list)=content.as_attribute_list() {
            for item in &list.entries {
                if item.attribute_type==MftAttributeType::FileName as u32 && item.segment_reference.entry!=record_number {
                    ensure!(extension_records.len()<1024,"MFT extension-record reference count exceeds budget");
                    extension_records.insert(reference(item.segment_reference.entry,item.segment_reference.sequence)?);
                }
            }
        }
    }
    let mut names=Vec::new();let mut seen=HashSet::new();let mut file_name_attributes=0usize;let mut dos_only_attributes=0usize;
    // Filter before decoding attribute values. DATA attributes (including
    // resident file contents) and all other user streams are never interpreted.
    for attribute in entry.iter_attributes_matching(Some(vec![MftAttributeType::FileName])) {
        let attribute=attribute?;
        let name=attribute.data.as_file_name().ok_or_else(||anyhow::anyhow!("FILE_NAME attribute did not decode"))?;
        file_name_attributes+=1;
        if matches!(&name.namespace,FileNamespace::DOS){dos_only_attributes+=1;continue}
        let units:Vec<_>=name.name.as_utf16le_bytes().chunks_exact(2).map(|b|u16::from_le_bytes([b[0],b[1]])).collect();
        ensure!(!units.is_empty() && units.len()<=255 && !units.iter().any(|u|*u==0 || *u==b'/' as u16 || *u==b'\\' as u16),"invalid NTFS filename component");
        ensure!(names.len()<1024,"FILE_NAME count exceeds budget");
        let parent=reference(name.parent.entry,name.parent.sequence)?;
        let mut bytes=Vec::new();let lossy=goz_core::wtf8::from_utf16(&units,&mut bytes);
        if seen.insert((parent,bytes.clone())) {names.push(LayoutName {parent_frn:parent,name:bytes,name_lossy:lossy,dos_only:false});}
    }
    Ok(Some(Fragment {record,owner,is_base,directory:entry.is_dir(),base_link_count:is_base.then_some(entry.header.hard_link_count),names,kernel_restored_fixups:kernel_restored,
        file_name_attributes,dos_only_attributes,has_attribute_list,nonresident_attribute_list,
        extension_records:extension_records.into_iter().collect()}))
}

#[cfg(test)]mod tests{
    use super::*;
    fn put16(raw:&mut[u8],at:usize,n:u16){raw[at..at+2].copy_from_slice(&n.to_le_bytes());}
    fn put32(raw:&mut[u8],at:usize,n:u32){raw[at..at+4].copy_from_slice(&n.to_le_bytes());}
    fn put64(raw:&mut[u8],at:usize,n:u64){raw[at..at+8].copy_from_slice(&n.to_le_bytes());}
    fn fixture(names:&[(u64,Vec<u16>,u8)],owner:u64)->Vec<u8>{
        let mut raw=vec![0;1024];raw[..4].copy_from_slice(b"FILE");
        put16(&mut raw,4,48);put16(&mut raw,6,3);put16(&mut raw,16,7);put16(&mut raw,18,2);
        put16(&mut raw,20,56);put16(&mut raw,22,1);put32(&mut raw,28,1024);put64(&mut raw,32,owner);
        let mut at=56;
        for(parent,name,namespace)in names{
            let value_len=66+name.len()*2;let len=(24+value_len+7)&!7;
            put32(&mut raw,at,0x30);put32(&mut raw,at+4,len as u32);put32(&mut raw,at+16,value_len as u32);put16(&mut raw,at+20,24);
            let value=at+24;put64(&mut raw,value,*parent);raw[value+64]=name.len() as u8;raw[value+65]=*namespace;
            for(i,u)in name.iter().enumerate(){put16(&mut raw,value+66+i*2,*u)}
            at+=len;
        }
        put32(&mut raw,at,u32::MAX);put32(&mut raw,24,(at+4)as u32);
        let first=u16::from_le_bytes(raw[510..512].try_into().unwrap());let second=u16::from_le_bytes(raw[1022..1024].try_into().unwrap());
        put16(&mut raw,48,0xABCD);put16(&mut raw,50,first);put16(&mut raw,52,second);put16(&mut raw,510,0xABCD);put16(&mut raw,1022,0xABCD);
        raw
    }
    #[test]fn decodes_every_non_dos_link_and_full_sequence_references(){
        let raw=fixture(&[(100,"LongName.exe".encode_utf16().collect(),1),(100,"LONGNA~1.EXE".encode_utf16().collect(),2),(101,"Alias.exe".encode_utf16().collect(),1)],0);
        let f=decode(&raw,200).unwrap().unwrap();assert!(f.is_base);assert_eq!(f.base_link_count,Some(2));assert_eq!(f.names.len(),2);
        assert_eq!(f.file_name_attributes,3);assert_eq!(f.dos_only_attributes,1);
        assert_eq!(f.record,Frn((7u64<<48)|200));assert_eq!(f.owner,f.record);assert_eq!(f.names[1].parent_frn,Frn(101));
    }
    #[test]fn preserves_unpaired_utf16_and_extension_ownership(){
        let units=vec![b'a' as u16,0xD800,b'.' as u16,b'e' as u16,b'x' as u16,b'e' as u16];
        let owner=(3u64<<48)|123;let raw=fixture(&[(100,units.clone(),1)],owner);
        let f=decode(&raw,200).unwrap().unwrap();assert!(!f.is_base);assert_eq!(f.owner,Frn(owner));assert_eq!(f.base_link_count,None);
        assert_eq!(goz_core::wtf8::to_utf16(&f.names[0].name),units);assert!(f.names[0].name_lossy);
    }
    #[test]fn rejects_torn_records_and_oversized_or_invalid_components(){
        let mut raw=fixture(&[(100,"test.exe".encode_utf16().collect(),1)],0);raw[510]^=1;
        assert!(decode(&raw,200).is_err());assert!(decode(&vec![0;131072],200).is_err());
        let invalid=fixture(&[(100,"bad\\name".encode_utf16().collect(),1)],0);assert!(decode(&invalid,200).is_err());
        assert!(decode(&vec![0;1024],200).unwrap().is_none());
    }
    #[test]fn kernel_restored_tails_are_explicit_and_mixed_records_are_rejected(){
        let mut raw=fixture(&[(100,"test.exe".encode_utf16().collect(),1)],0);
        let first=raw[50..52].to_vec();let second=raw[52..54].to_vec();
        raw[510..512].copy_from_slice(&first);
        assert!(decode_fsctl(&raw,200).is_err());
        raw[1022..1024].copy_from_slice(&second);
        assert!(decode(&raw,200).is_err());
        assert!(decode_fsctl(&raw,200).unwrap().unwrap().kernel_restored_fixups);
    }
    #[test]fn hardlink_audit_requires_base_and_all_name_fragments(){
        let base=decode(&fixture(&[(100,"first.exe".encode_utf16().collect(),1)],0),200).unwrap().unwrap();
        let extension=decode(&fixture(&[(101,"second.exe".encode_utf16().collect(),1)],base.record.0),201).unwrap().unwrap();
        let mut audit=LinkAudit::default();audit.observe(&base).unwrap();
        assert!(audit.finish().is_err());
        audit.observe(&extension).unwrap();audit.finish().unwrap();
    }
    #[test]fn hardlink_audit_checkpoint_resumes_between_base_and_extension_records(){
        let base=decode(&fixture(&[(100,"first.exe".encode_utf16().collect(),1)],0),200).unwrap().unwrap();
        let extension=decode(&fixture(&[(101,"second.exe".encode_utf16().collect(),1)],base.record.0),201).unwrap().unwrap();
        let mut audit=LinkAudit::default();audit.observe(&base).unwrap();
        let bytes=serde_json::to_vec(&audit.checkpoint()).unwrap();
        let saved:LinkAuditCheckpoint=serde_json::from_slice(&bytes).unwrap();
        let mut resumed=LinkAudit::from_checkpoint(saved).unwrap();
        assert!(resumed.finish().is_err());
        resumed.observe(&extension).unwrap();resumed.finish().unwrap();
    }
    #[test]fn hardlink_audit_accounts_for_non_searchable_dos_name_attributes(){
        let raw=fixture(&[(100,"program.exe".encode_utf16().collect(),1),(100,"PROGRA~1.EXE".encode_utf16().collect(),2)],0);
        let fragment=decode(&raw,200).unwrap().unwrap();
        assert_eq!(fragment.names.len(),1);assert_eq!(fragment.file_name_attributes,2);assert_eq!(fragment.dos_only_attributes,1);
        let mut audit=LinkAudit::default();audit.observe(&fragment).unwrap();audit.finish().unwrap();
    }
    #[test]fn hardlink_audit_rejects_duplicate_bases_and_unowned_extensions(){
        let base_raw=fixture(&[(100,"first.exe".encode_utf16().collect(),1),(101,"second.exe".encode_utf16().collect(),1)],0);
        let base=decode(&base_raw,200).unwrap().unwrap();
        let mut audit=LinkAudit::default();audit.observe(&base).unwrap();
        assert!(audit.observe(&base).is_err());
        let extension=decode(&fixture(&[(100,"third.exe".encode_utf16().collect(),1)],base.record.0),201).unwrap().unwrap();
        let mut orphan=LinkAudit::default();orphan.observe(&extension).unwrap();assert!(orphan.finish().is_err());
        assert!(audit.observe(&extension).is_err());
    }
}
