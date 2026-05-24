# GTStore System 

**Date: 2025-11-28**
**Authors: Barnabe Marty, Padraig Littlefield**

The GTStore system is a cluster node gRPC system that implements Dynamo-like key-value pair storage with consistency, durability, and failure mitigation.

There are many ways to run the GTStore system to see the various capabilities and how they affect everything.

Compiling the code is done as such:
1. Type `make` to compile the GTStore system (this includes proto lib files and object files)
2. Type `chmod +x *.sh` in the project folder to make .sh scripts executable

First to run standard tests 1-4:
1. Execute any of the test scripts: `./test<#>.sh`

OR run manager and storage nodes, with client your own way:
1. Type `./start_service.sh --nodes <# of nodes> --rep <K rep factor>` to start the services
2. Use client interactively, `./bin/client --interactive`, OR use it with terminal, `./bin/client --put <key> --val <value>` AND `./bin/client --get <key>`
3. To test node failure, feel free to `kill -9 <PID>` any of the storage nodes (wait about 20 seconds for system to stabilize)
4. When done, run `./stop_service.sh` to kill storage and manager nodes