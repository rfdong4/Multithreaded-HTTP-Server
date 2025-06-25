# Multithreaded HTTP Server

This project implements a multithreaded HTTP server in C. It supports concurrent handling of HTTP GET and PUT requests, using a thread pool and per-URI reader-writer locks for safe file access.

## Directory Structure

- `multithreaded-httpserver/` — Main source code and build directory
  - `httpserver.c` — Main server implementation
  - `*.h` — Header files for server modules
  - `Makefile` — Build instructions
  - `test_repo.sh` — Script to run all test scripts
  - `test_scripts/` — Test scripts (.sh, .py)
  - `test_files/` — Test data files
  - `workloads/` — Workload configuration files
  - `replay/`, `.format/` — Generated or temporary files (ignored by git)

## Building the Server

From the `multithreaded-httpserver/` directory, run:

```
make
```

This will produce the `httpserver` binary.

To clean build artifacts:

```
make clean
```

## Running the Server

```
./httpserver [-t THREADS] <PORT>
```
- `-t THREADS` (optional): Number of worker threads (default: 4)
- `<PORT>`: Port number to listen on

Example:
```
./httpserver -t 8 8080
```

## Features
- Handles HTTP GET and PUT requests
- Thread pool for concurrent connections
- Per-URI reader-writer locks for safe file access
- Audit logging to stderr

## Testing

To run all test scripts:

```
./test_repo.sh
```

This will execute all `.sh` scripts in `test_scripts/` (except utility scripts) and report their results.

## Notes
- All build artifacts, outputs, and temporary files are ignored by git (see `.gitignore`).
- Only source code, headers, scripts, and documentation are tracked.

---

Use this README to document design notes, testing strategies, or questions as you develop the project.
