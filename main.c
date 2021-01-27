#include <stdio.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <zlib.h>

#ifdef __linux__
#include <wait.h>
#include <string.h>
#include <fcntl.h>
#elif __APPLE__
#include <string.h>
#include <libc.h>
#endif

// Used to hold file descriptors so we can pass less parameters around.
struct FileDescriptors {
    int dupedIn;
    int dupedOut;
    int dupedErr;
    int command;
};

// Exit the process (report an error if the exitCode != 0).
void errExit(struct FileDescriptors *descriptors, int exitCode, int error, char* msg) {
    // This method can physically only be called either
    // (a) before forking or (b) if forking failed.
    // In both cases we are safe to write to STDERR ... so
    // Print an error message to STDERR.
    if (exitCode != 0) {
        if (msg) {
            dprintf(STDERR_FILENO, "FAILED: errno = %i, Message = %s\n", error, msg);
        }
        else {
            dprintf(STDERR_FILENO, "FAILED: errno = %i\n", error);
        }
    }
    // Close all known open descriptors.
    if (descriptors->dupedIn >= 0) {
        close(descriptors->dupedIn);
    }
    if (descriptors->dupedOut >= 0) {
        close(descriptors->dupedOut);
    }
    if (descriptors->dupedErr >= 0) {
        close(descriptors->dupedErr);
    }
    if (descriptors->command >= 0) {
        close(descriptors->command);
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    // and exit.
    exit(exitCode);
}

// Exit successfully with no error ...
// this is a convenience function around: errExit(descriptors, 0, 0, "")
void closeAndExit(struct FileDescriptors *descriptors) {
    // Exit successfully
    errExit(descriptors, 0, 0, "");
}

// Test if string a starts with string b
int contains(const char *a, const char *b)
{
    if(strstr(a, b) != 0) {
        return 1;
    }
    return 0;
}

// Main entry point
int main(int argc, char** argv) {
    // Duplicate the STD file descriptors so we can use
    // them as part of fork.
    struct FileDescriptors descriptors;
    descriptors.dupedIn = dup(STDIN_FILENO);
    descriptors.dupedOut = dup(STDOUT_FILENO);
    descriptors.dupedErr = dup(STDERR_FILENO);

    // verify we have arguments (such as a command to run)
    // If no arguments exit.
    // NOTE: argv[0] is the path to this file so it's not valuable and
    // ignored so we need: argc > 1
    if (argc <= 1) {
        errExit(&descriptors, 6, 0,
                "Insufficient arguments.  No command supplied to execute.\n");
    }

    int execvStart = 1;
    int execvSize = argc;
    
    // Argv[1] could be the command channel.  If so, then we start params
    // with argv[2].

    // The command channel is a fifo pipe (mkfifo) that may have been created by
    // the caller.  If this channel was created, the name of the fifo pipe will be
    // supplied via argv[1] (and only argv[1]) which we can determine by argv[1]
    // starting with "cc:".
    // If no command channel was created it will not be opened and will not be used.
    // However, if it is present we will send the pid and the ppid (as this is
    // otherwise difficult to compute) down the channel for the caller to read.
    char *commandChannel = 0;
    if (contains(argv[1], ":cc:")) {
        commandChannel = argv[1];
        // move execvStart to 2
        execvStart++;
        // decrease execvSize
        execvSize--;

        // if a command channel was specified we need to check again for
        // sufficient arguments.
        if (argc <= 2) {
            errExit(&descriptors, 6, 0,
                    "Insufficient arguments.  No command supplied to execute (command channel variant).\n");
        }
    }

    // The first element of argv is the path to this application ...
    // this will be discarded.  The last element of the args array
    // needs to be a NULL value.
    // The net result is the size of the array we need to pass to
    // execv is the same as the size of the supplied args.
    // We only need to shift the pointers.

    // If a command channel is supplied the mapping should look like this.
    //                   argv[0]            // path to this file ... not needed for execv
    //                   argv[1]            // the command channel ... not needed for execv
    // args[0]         = argv[2]            // the name of the file being called.
    // args[1 ... n]   = argv[3 ... n+2]    // the arguments for the command being execv'ed
    // args[n+1]       = NULL               // the execv null terminator.

    // If a command channel is not supplied the mapping should look like this.
    // NOTE: In this case we malloc too much space, but that should be okay for our purpose.
    //                   argv[0]            // path to this file ... not needed for execv
    // args[0]         = argv[1]            // the name of the file being called.
    // args[1 ... n]   = argv[2 ... n+1]    // the arguments for the command being execv'ed
    // args[n+1]       = NULL               // the execv null terminator.
    char** args = (char**)malloc(sizeof(char*) * (argc) );
    for(int i = execvStart; i < argc; i++) {
        args[i - execvStart] = argv[i];
    }
    // excv needs a null terminated string array so the last arg
    // needs to be a null pointer.
    args[execvSize] = 0;

    descriptors.command = -1;
    if (commandChannel) {
        descriptors.command = open(commandChannel, O_WRONLY);
        if (descriptors.command <= 0) {
            errExit(&descriptors, 5, errno, "Unable to open command Channel.");
        }
    }

    // Fork the process.
    pid_t pid = fork();
    if (pid == -1) {
        // Fork failed ... send error msg and exit.
        errExit(&descriptors, 4, 0, "Unable to fork process exiting.\n");
    }
    // The fork succeeded the two process will continue from
    // this point with different values for pid.
    // The forked (created) process will receive pid = 0.
    // The forking (original) process will receive the pid of the forked (created) process.
    else if (pid == 0) {
        // CHILD FORK
        // Get the actual pid of this process.
        pid = getpid();

        // Send the Process Id to the command channel.
        if (descriptors.command >= 0) {
            dprintf(descriptors.command, "%d\n", pid);
        }
        if (descriptors.command >= 0) {
            dprintf(descriptors.command, "%d\n", getppid());
        }

        // Detach the fork from the parent group ... this allows
        // the process to live past it's caller.
        int sid = setsid();
        if (sid != pid) {
            // detachment failed... error and exit.
            errExit(&descriptors, 3, errno, "Unable to create session");
        }

        // calling setsid() changed the STD streams from the original (forking) process.
        // we will dup them back to the file descriptors we originally created at the
        // beginning so we can write back to the STD streams of the forking (original) process.
        int dr = dup2(descriptors.dupedIn, STDIN_FILENO);
        if (dr != STDIN_FILENO) {
            // dup failed ... error and exit.
            errExit(&descriptors, 2, errno, "Unable to dup STDIN");
        }
        dr = dup2(descriptors.dupedOut, STDOUT_FILENO);
        if (dr != STDOUT_FILENO) {
            // dup failed ... error and exit.
            errExit(&descriptors, 2, errno, "Unable to dup STDOUT");
        }
        dr = dup2(descriptors.dupedErr, STDERR_FILENO);
        if (dr != STDERR_FILENO) {
            // dup failed ... error and exit.
            errExit(&descriptors, 2, errno, "Unable to dup STDERR");
        }
        // now that we have duped the supplied fds into our STD fds ... we
        // can close the supplied fds as we will use the STD fds from here on out.
        close(descriptors.dupedIn);
        close(descriptors.dupedOut);
        close(descriptors.dupedErr);

        // this will load the file into memory and execute...
        // We should only receive a result from this call if there is an error.
        execv(args[0], args);
        // execv failed ... error and exit.
        errExit(&descriptors, 1, errno, "Unable to exec process");
    } else {
        // PARENT FORK
//        int status = 0;
//        // wait for the forked (created) process to complete.
//        waitpid(pid, &status, 0);
//        // close and exit successfully.
//        if (descriptors.command >= 0) {
//            dprintf(descriptors.command, "%d\n", WEXITSTATUS(status));
//        }

        closeAndExit(&descriptors);
    }
}
