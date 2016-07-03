# TurboVAX 

## *This is NOT SIMH!*
This project is _based_ on SIMH as that project wonderfully supports solid, basic and easily understood. That projects aim is to provide a simulator for early minicomputers. SIMH's goals are to ensure that you will be able to simulate its range of machines on any _new_ machine at any point in the future.

This is a superb goal, you never will know when you might bump into a running PDP-11 in 30 years time and will need to emulate it on your then current computer. Keeping SIMH in simple C will keep with this goal and allow such machines to be studied right from the source code at any point in the future.

That means that SIMH curtails using more exotic features and designs, such as 

- More advanced, and specific OS support
- Using more advanced CPU features
- JIT backending

This project is designed different, the goal is to make the fastest microvax3900 emulator possible in open-source.

## Rough Roadmap
- Upgrade interpretor to use Computed Gotos / GCC labels
- Rework interpretor parts to use hand rolled ASM instruction implementations
- Evaluate inline dispatching in the interpreter
- Look into devising a "micro-risc" for the VAX ISA, like the i64 architecture has
- Evaluate morphing VAX-ISA to micro-risc, with micro-risc being an IR for a JIT
- Implement basic intrinsics for said JIT
- Implement basic SSA and linear scan allocator optimisations for a JIT
- Implement aggressive optimisations and graph coloring in said JIT
- Look into OS support for HugePages, Nested Pages and other memory mapping structures
- Vectorise the VAX

## Building simulators yourself
First download clone the latest source code, I dont make releases presently.
Depending on your host platform one of the following steps should be followed:

### Linux/OSX/*BSD/*nix platforms

If you are interested in using a simulator with Ethernet networking support (i.e. one of the VAX simulators or the PDP11), then you should make sure you have the correct networking components available.  The instructions in https://github.com/simh/simh/blob/master/0readme_ethernet.txt describe the required steps to get ethernet networking components installed and how to configure your environment.

See the 0readme_ethernet.txt file for details about the required network components for your platform.  Once your operating system build environment has the correct networking components available the following command will build working simulators:

   $ make {simulator-name (i.e. vax)}

#### Build Dependencies

Some simulators depend on external packages to provide the full scope of functionality they may be simulating.  These additional external packages may or may not be included in as part of the standard Operating System distributions.  

##### OS X - Dependencies

The MacPorts package manager is available to provide these external packages.  Once MacPorts is installed, these commands will install the required dependent packages:

    # port install vde2
    # port install libsdl2

##### Linux - Dependencies

Different Linux distributions have different package managment systems:

Ubuntu:

    # apt-get install libpcap-dev
    # apt-get install libvdeplug-dev
    # apt-get install vde2
    # apt-get install libsdl2

## Problem Reports

*Dont report bugs to SIMH!*
If you find problems please report these using the github "Issue" interface at https://github.com/GregBowyer/simh/issues.

Problem reports should contain;
 - a description of the problem
 - the simulator you experience the problem with
 - your host platform (and OS version)
 - how you built the simulator or that you're using prebuilt binaries
 - the simulator build description should include the output produced by while building the simulator
 - the output of SHOW VERSION while running the simulator which is having an issue
 - the simulator configuration file (or commands) which were used when the problem occurred.
 
