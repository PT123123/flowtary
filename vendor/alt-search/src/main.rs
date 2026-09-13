mod cli;

use clap::Parser;
use cli::Cli;

fn main() {
    let cli = Cli::parse();
    if cli.serve {
        altsearch::daemon::serve(&cli.dir, cli.reindex);
    } else {
        cli::run(&cli);
    }
}
