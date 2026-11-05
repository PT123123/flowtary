//! Persistent query transport. Loading happens once, never on the keystroke path.
//! This unprivileged snapshot host does not open volumes or auto-start collection.
use crate::{catalog::{Catalog, Query, QueryResult}, policy::Cancellation};
use anyhow::{Result, ensure};
use serde::Deserialize;
use std::{io::{BufRead, Write}, sync::RwLock};

const MAX_REQUEST_BYTES: usize = 64 * 1024;
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request { schema:u32, id:u64, query:Query }

#[cfg(test)]
pub fn serve(catalog:&Catalog, mut input:impl BufRead, mut output:impl Write,
             cancel:&Cancellation) -> Result<()> {
    serve_authorized(catalog, &mut input, &mut output, cancel, || Ok(()))
}

/// Serves the same bounded query protocol while authorizing each complete
/// request before parsing or executing it. Privileged named-pipe servers must
/// re-check the impersonated client SID for every request because a connected
/// client thread can change its impersonation token between writes.
#[cfg(test)]
pub fn serve_authorized(
    catalog: &Catalog,
    input: impl BufRead,
    output: impl Write,
    cancel: &Cancellation,
    authorize_request: impl FnMut() -> Result<()>,
) -> Result<()> {
    serve_inner(input,output,cancel,authorize_request,|query|catalog.query(query,cancel))
}

/// Serves requests against a catalog that can be replaced by a background
/// snapshot reloader. The read lock is held only for one bounded query; loading
/// a new generation happens outside the lock.
pub fn serve_shared(
    catalog:&RwLock<Catalog>,
    input:impl BufRead,
    output:impl Write,
    cancel:&Cancellation,
    authorize_request:impl FnMut()->Result<()>,
)->Result<()> {
    serve_inner(input,output,cancel,authorize_request,|query|{
        let current=catalog.read().unwrap_or_else(|poisoned|poisoned.into_inner());
        current.query(query,cancel)
    })
}

fn serve_inner(
    mut input:impl BufRead,
    mut output:impl Write,
    cancel:&Cancellation,
    mut authorize_request:impl FnMut()->Result<()>,
    mut execute:impl FnMut(&Query)->Result<QueryResult>,
)->Result<()> {
    // The client waits for this frame before sending its first search. Snapshot
    // loading can take seconds at multi-million-entry scale; warm requests keep
    // their separate short transport deadline.
    output.write_all(b"{\"schema\":1,\"ready\":true}\n")?;
    output.flush()?;
    let mut line=Vec::new();
    loop {
        cancel.check()?;
        line.clear();
        // take() bounds allocation even for a peer that sends no newline.
        let count=std::io::Read::take(&mut input,(MAX_REQUEST_BYTES+1) as u64).read_until(b'\n',&mut line)?;
        if count==0 { return Ok(()); }
        ensure!(count<=MAX_REQUEST_BYTES,"query request exceeds 64 KiB; transport closed");
        authorize_request()?;
        let response=match serde_json::from_slice::<Request>(&line) {
            Ok(request) => {
                let result=(|| {
                    ensure!(request.schema==1,"unsupported query protocol schema");
                    execute(&request.query)
                })();
                match result {
                    Ok(result)=>serde_json::json!({"schema":1,"id":request.id,"result":result}),
                    Err(error)=>serde_json::json!({"schema":1,"id":request.id,"error":format!("{error:#}")}),
                }
            }
            Err(error)=>serde_json::json!({"schema":1,"id":null,"error":format!("invalid request: {error}")}),
        };
        serde_json::to_writer(&mut output,&response)?;
        output.write_all(b"\n")?; output.flush()?;
    }
}

#[cfg(test)] mod tests {
    use super::*;
    #[test] fn persistent_protocol_recovers_after_bad_queries_and_bounds_input() {
        let c=crate::catalog::tests::fixture(); let cancel=Cancellation::default();
        let input=b"invalid\n{\"schema\":1,\"id\":8,\"query\":{\"words\":[\"aura\"],\"workshop_only\":true}}\n{\"schema\":1,\"id\":9,\"query\":{\"words\":[],\"limit\":0}}\n";
        let mut output=Vec::new(); serve(&c,&input[..],&mut output,&cancel).unwrap();
        let lines:Vec<serde_json::Value>=String::from_utf8(output).unwrap().lines().map(|s|serde_json::from_str(s).unwrap()).collect();
        assert_eq!(lines.len(),4); assert_eq!(lines[0]["ready"],true);
        assert!(lines[1]["error"].is_string());
        assert_eq!(lines[2]["id"],8); assert_eq!(lines[2]["result"]["hits"].as_array().unwrap().len(),3);
        assert!(lines[3]["error"].is_string());
        let mut output=Vec::new();assert!(serve(&c,&vec![b'x';MAX_REQUEST_BYTES+1][..],&mut output,&cancel).is_err());
        assert_eq!(output.iter().filter(|&&b|b==b'\n').count(),1);
    }

    #[test]
    fn authorization_runs_after_each_frame_is_read_and_before_it_is_processed() {
        let c = crate::catalog::tests::fixture();
        let cancel = Cancellation::default();
        let input = b"invalid\n{\"schema\":1,\"id\":8,\"query\":{\"words\":[\"aura\"],\"workshop_only\":true}}\n";
        let mut output = Vec::new();
        let mut calls = 0;
        let result = serve_authorized(&c, &input[..], &mut output, &cancel, || {
            calls += 1;
            anyhow::bail!("client SID rejected")
        });
        assert!(result.unwrap_err().to_string().contains("client SID rejected"));
        assert_eq!(calls, 1, "a rejected frame must not be processed or followed by more requests");
        let output = String::from_utf8(output).unwrap();
        let lines: Vec<_> = output.lines().collect();
        assert_eq!(lines, ["{\"schema\":1,\"ready\":true}"]);
    }

    #[test]
    fn shared_session_observes_a_catalog_generation_swap_between_requests() {
        let catalog=RwLock::new(crate::catalog::tests::fixture());
        let cancel=Cancellation::default();
        let input=b"{\"schema\":1,\"id\":1,\"query\":{\"words\":[\"aura\"],\"workshop_only\":true}}\n{\"schema\":1,\"id\":2,\"query\":{\"words\":[\"aura\"],\"workshop_only\":true}}\n";
        let mut output=Vec::new();let mut requests=0;
        serve_shared(&catalog,&input[..],&mut output,&cancel,||{
            requests+=1;
            if requests==2 {catalog.write().unwrap().meta.stale=true;}
            Ok(())
        }).unwrap();
        let lines:Vec<serde_json::Value>=String::from_utf8(output).unwrap().lines()
            .map(|line|serde_json::from_str(line).unwrap()).collect();
        assert_eq!(lines.len(),3);
        assert_eq!(lines[1]["result"]["stale"],false);
        assert_eq!(lines[2]["result"]["stale"],true,
            "the session must query the swapped catalog rather than retain a stale borrowed generation");
    }
}
