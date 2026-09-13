mod cli;

use clap::Parser;
use cli::Cli;

fn main() {
    let cli = Cli::parse();
    if cli.serve {
        altsearch::daemon::serve(&cli.dir, cli.reindex, cli.port, cli.threads, cli.idle_exit_secs);
    } else {
        cli::run(&cli);
    }
}
