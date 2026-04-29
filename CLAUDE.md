# Job Orchestration and Broker Utility (JobU)

Job Queue system that supports:

* Multiple job queues
* Jobs with scheduled execution time; or recurring jobs using cron-like schedule; or jobs that execute immediately
* Jobs can be dispatched to any of the queues and queue keeps track of their execution
* Queues can be suspended and resumed
* Queues can have multiple runners allowing multiple jobs to run in parallel in the same queue
* Jobs can be CLI jobs running a command on the same machine; or HTTP jobs making HTTP requests to a remote server

## Architecture

- Core libraries and application in `/src`, unit-tests in `/tests`.

## Key Commands

- Build: `cmake -B .bld -DCMAKE_BUILD_TYPE=Debug && cmake --build .bld`
- Test: `ctest --test-dir .bld/test`
