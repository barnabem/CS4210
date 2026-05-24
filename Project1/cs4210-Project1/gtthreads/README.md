# GTThreads Library with Priority and Credit Scheduler

**Date: 2025-09-17**
**Author: Barnabe Marty**

The gtthreads library is a simple user-space threading library that tries to view the effects of different schedulers and modules on a system as a whole.

There are many was to run the gtthreads library to see what its various capabilities ae and how they affect the system.

Compiling the code is done as such:
1. Type 'make' to compile the GTThreads library 
2. Type 'make matrix' to compile the matrix program

First to run the default code (priority scheduler with no load balancing) do the following:
- Type ./bin/matrix to run the matrix program

If you want to specify the scheduler you may add '-s' as an arg when running the executable:
- ./bin/matrix -s 0 to run the program with priority scheduler
- ./bin/matrix -s 1 to run program with credit scheduler

To enable the load balancing of the system simply run:
- ./bin/matrix -lb 
(Note: you may specify a scheduler as well, i.e. ./bin/matrix -s 1 -lb will run credit scheduler with load balancing)