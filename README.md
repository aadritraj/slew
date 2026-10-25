# slew

> [!WARNING]
> While efforts to vet the code being generated have been made, most of this codebase was designed by an agent, with certain modifications by me.

slew is a lightweight, high-performance HTTP/1.1 web server implemented in modern C++20. It uses Linux `io_uring`. (bring kernel 5.19 or greater)


## Serve the example
```bash
make
./bin/slew
```

## CLI Options
Flags 

| Flag | Flag (Long) | Description | Default |
|------|-----------|-------------|---------|
| `-p` | `--port <num>` | Server port | `8080` |
| `-t` | `--threads <num>` | Amount of worker threads. (`0` = CPU core count) | `0` |
| `-d` | `--docroot <path>` | Static file roots | `./public` |
| `-s` | `--timeout <sec>` | Idle connection timeout (seconds) | `5` |
| `-h` | `--help` | Help text | `N/A` |

## Run tests
```bash
make test
```
