/* 
 * Copyright (c) 2015-2016, Gregory M. Kurtzer. All rights reserved.
 * 
 * “Singularity” Copyright (c) 2016, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).  All rights reserved.
 * 
 * If you have questions about your rights to use or distribute this software,
 * please contact Berkeley Lab's Innovation & Partnerships Office at
 * IPO@lbl.gov.
 * 
 * NOTICE.  This Software was developed under funding from the U.S. Department of
 * Energy and the U.S. Government consequently retains certain rights. As such,
 * the U.S. Government has been granted for itself and others acting on its
 * behalf a paid-up, nonexclusive, irrevocable, worldwide license in the Software
 * to reproduce, distribute copies to the public, prepare derivative works, and
 * perform publicly and display publicly, and to permit other to do so. 
 * 
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <errno.h> 
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <fcntl.h>  
#include <grp.h>
#include <libgen.h>

#include "config.h"
#include "mounts.h"
#include "loop-control.h"
#include "util.h"
#include "user.h"


#ifndef LIBEXECDIR
#define LIBEXECDIR "undefined"
#endif
#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif
#ifndef LOCALSTATEDIR
#define LOCALSTATEDIR "/var/"
#endif

// Yes, I know... Global variables suck but necessary to pass sig to child
pid_t child_pid = 0;


void sighandler(int sig) {
    signal(sig, sighandler);

    printf("Caught signal: %d\n", sig);
    fflush(stdout);

    if ( child_pid > 0 ) {
        printf("Singularity is sending SIGKILL to child pid: %d\n", child_pid);
        fflush(stdout);

        kill(child_pid, SIGKILL);
    }
}


int main(int argc, char ** argv) {
    char *containerimage;
    char *containername;
    char *containerpath;
    char *homepath;
    char *command;
    char *command_exec;
    char *tmpdir;
    char *lockfile;
    char *loop_dev_cache;
    char *loop_dev = 0;
    char *basehomepath;
    char cwd[PATH_MAX];
    int cwd_fd;
    int tmpdirlock_fd;
    int containerimage_fd;
    int lockfile_fd;
    int retval = 0;
    uid_t uid = getuid();
    gid_t gid = getgid();


//****************************************************************************//
// Init                                                                       //
//****************************************************************************//

    // Lets start off as the calling UID
    if ( seteuid(uid) < 0 ) {
        fprintf(stderr, "ABORT: Could not set effective user privledges to %d!\n", uid);
        return(255);
    }

    homepath = getenv("HOME");
    containerimage = getenv("SINGULARITY_IMAGE");
    command = getenv("SINGULARITY_COMMAND");
    command_exec = getenv("SINGULARITY_EXEC");

    unsetenv("SINGULARITY_IMAGE");
    unsetenv("SINGULARITY_COMMAND");
    unsetenv("SINGULARITY_EXEC");

    // Figure out where we start
    if ( (cwd_fd = open(".", O_RDONLY)) < 0 ) {
        fprintf(stderr, "ABORT: Could not open cwd fd (%s)!\n", strerror(errno));
        return(1);
    }
    if ( getcwd(cwd, PATH_MAX) == NULL ) {
        fprintf(stderr, "Could not obtain current directory path\n");
        return(1);
    }

    if ( containerimage == NULL ) {
        fprintf(stderr, "ABORT: SINGULARITY_IMAGE undefined!\n");
        return(1);
    }

    if ( is_file(containerimage) != 0 ) {
        fprintf(stderr, "ABORT: Container image path is invalid: %s\n", containerimage);
        return(1);
    }

    if ( is_dir(homepath) != 0 ) {
        fprintf(stderr, "ABORT: Home directory not found: %s\n", homepath);
        return(1);
    }

    if ( is_owner(homepath, uid) != 0 ) {
        fprintf(stderr, "ABORT: You don't own your own home directory!?: %s\n", homepath);
        return(1);
    }

    // TODO: Offer option to only run containers owned by root (so root can approve
    // containers)
    if ( is_owner(containerimage, uid) < 0 && is_owner(containerimage, 0) < 0 ) {
        fprintf(stderr, "ABORT: Will not execute in a CONTAINERIMAGE you (or root) does not own: %s\n", containerimage);
        return(255);
    }

    containername = basename(strdup(containerimage));
    basehomepath = strjoin("/", strtok(strdup(homepath), "/"));

    containerpath = (char *) malloc(strlen(LOCALSTATEDIR) + 18);
    snprintf(containerpath, strlen(LOCALSTATEDIR) + 18, "%s/singularity/mnt", LOCALSTATEDIR);

    tmpdir = strjoin("/tmp/.singularity-", file_id(containerimage));
    lockfile = joinpath(tmpdir, "lock");
    loop_dev_cache = joinpath(tmpdir, "loop_dev");


//****************************************************************************//
// Setup                                                                      //
//****************************************************************************//

    if ( s_mkpath(tmpdir, 0750) < 0 ) {
        fprintf(stderr, "ABORT: Could not temporary directory %s: %s\n", tmpdir, strerror(errno));
        return(255);
    }

    tmpdirlock_fd = open(tmpdir, O_RDONLY);
    if ( tmpdirlock_fd < 0 ) {
        fprintf(stderr, "ERROR: Could not create lock file %s: %s\n", lockfile, strerror(errno));
        return(255);
    }
    if ( flock(tmpdirlock_fd, LOCK_SH | LOCK_NB) < 0 ) {
        fprintf(stderr, "ERROR: Could not obtain shared lock on %s: %s\n", lockfile, strerror(errno));
        return(255);
    }

    if ( ( lockfile_fd = open(lockfile, O_CREAT | O_RDWR, 0644) ) < 0 ) {
        fprintf(stderr, "ERROR: Could not open lockfile %s: %s\n", lockfile, strerror(errno));
        return(255);
    }

    // When we contain, we need temporary directories for what should be writable
    if ( getenv("SINGULARITY_CONTAIN") != NULL ) {
        if ( s_mkpath(joinpath(tmpdir, homepath), 0750) < 0 ) {
            fprintf(stderr, "ABORT: Failed creating temporary directory %s: %s\n", joinpath(tmpdir, homepath), strerror(errno));
            return(255);
        }
        if ( s_mkpath(joinpath(tmpdir, "/tmp"), 0750) < 0 ) {
            fprintf(stderr, "ABORT: Failed creating temporary directory %s: %s\n", joinpath(tmpdir, "/tmp"), strerror(errno));
            return(255);
        }
    }

    if ( seteuid(0) < 0 ) {
        fprintf(stderr, "ABORT: Could not escalate effective user privledges!\n");
        return(255);
    }

    if ( is_dir(containerpath) < 0 ) {
        if ( s_mkpath(containerpath, 0755) < 0 ) {
            fprintf(stderr, "ABORT: Could not create directory %s: %s\n", containerpath, strerror(errno));
            return(255);
        }
    }


//****************************************************************************//
// Setup namespaces                                                           //
//****************************************************************************//

    if ( unshare(CLONE_NEWNS) < 0 ) {
        fprintf(stderr, "ABORT: Could not virtulize mount namespace\n");
        return(255);
    }

    // Privitize the mount namespaces (thank you for the pointer Doug Jacobsen!)
    if ( mount(NULL, "/", NULL, MS_PRIVATE|MS_REC, NULL) < 0 ) {
        // I am not sure if this error needs to be caught, maybe it will fail
        // on older kernels? If so, we can fix then.
        fprintf(stderr, "ABORT: Could not make mountspaces private: %s\n", strerror(errno));
        return(255);
    }

#ifdef NS_CLONE_NEWPID
    if ( getenv("SINGULARITY_NO_NAMESPACE_PID") == NULL ) {
        unsetenv("SINGULARITY_NO_NAMESPACE_PID");
        if ( unshare(CLONE_NEWPID) < 0 ) {
            fprintf(stderr, "ABORT: Could not virtulize PID namespace\n");
            return(255);
        }
    }
#else
#ifdef NS_CLONE_PID
    // This is for older legacy CLONE_PID
    if ( getenv("SINGULARITY_NO_NAMESPACE_PID") == NULL ) {
        unsetenv("SINGULARITY_NO_NAMESPACE_PID");
        if ( unshare(CLONE_PID) < 0 ) {
            fprintf(stderr, "ABORT: Could not virtulize PID namespace\n");
            return(255);
        }
    }
#endif
#endif
#ifdef NS_CLONE_FS
    if ( getenv("SINGULARITY_NO_NAMESPACE_FS") == NULL ) {
        unsetenv("SINGULARITY_NO_NAMESPACE_FS");
        if ( unshare(CLONE_FS) < 0 ) {
            fprintf(stderr, "ABORT: Could not virtulize file system namespace\n");
            return(255);
        }
    }
#endif
#ifdef NS_CLONE_FILES
    if ( getenv("SINGULARITY_NO_NAMESPACE_FILES") == NULL ) {
        unsetenv("SINGULARITY_NO_NAMESPACE_FILES");
        if ( unshare(CLONE_FILES) < 0 ) {
            fprintf(stderr, "ABORT: Could not virtulize file descriptor namespace\n");
            return(255);
        }
    }
#endif


//****************************************************************************//
// Mount image                                                                //
//****************************************************************************//

    if ( ( containerimage_fd = open(containerimage, O_RDWR) ) < 0 ) {
        fprintf(stderr, "ERROR: Could not open image %s: %s\n", containerimage, strerror(errno));
        return(255);
    }

    if ( flock(lockfile_fd, LOCK_EX | LOCK_NB) == 0 ) {
        loop_dev = obtain_loop_dev();
        if ( associate_loop(containerimage_fd, loop_dev) < 0 ) {
            fprintf(stderr, "ERROR: Could not associate %s to loop device %s\n", containerimage, loop_dev);
            return(255);
        }

        if ( fileput(loop_dev_cache, loop_dev) < 0 ) {
            fprintf(stderr, "ERROR: Could not write to loop_dev_cache %s: %s\n", loop_dev_cache, strerror(errno));
            return(255);
        }
        flock(lockfile_fd, LOCK_SH | LOCK_NB);

    } else {
        flock(lockfile_fd, LOCK_SH);
        if ( ( loop_dev = filecat(loop_dev_cache) ) == NULL ) {
            fprintf(stderr, "ERROR: Could not retrieve loop_dev_cache from %s\n", loop_dev_cache);
            return(255);
        }
    }

    if ( getenv("SINGULARITY_WRITABLE") == NULL ) {
        unsetenv("SINGULARITY_WRITABLE");
        if ( flock(containerimage_fd, LOCK_SH | LOCK_NB) < 0 ) {
            fprintf(stderr, "ABORT: Image is locked by another process\n");
            return(5);
        }
        if ( mount_image(loop_dev, containerpath, 0) < 0 ) {
            fprintf(stderr, "ABORT: exiting...\n");
            return(255);
        }
    } else {
        if ( flock(containerimage_fd, LOCK_EX | LOCK_NB) < 0 ) {
            fprintf(stderr, "ABORT: Image is locked by another process\n");
            return(5);
        }
        if ( mount_image(loop_dev, containerpath, 1) < 0 ) {
            fprintf(stderr, "ABORT: exiting...\n");
            return(255);
        }
    }


//****************************************************************************//
// Drop privileges for temporary file generation                              //
//****************************************************************************//

    if ( seteuid(uid) < 0 ) {
        fprintf(stderr, "ABORT: Could not drop effective user privledges!\n");
        return(255);
    }

    if ( is_file(joinpath(tmpdir, "/passwd")) < 0 ) {
        if ( build_passwd(joinpath(containerpath, "/etc/passwd"), joinpath(tmpdir, "/passwd")) < 0 ) {
            fprintf(stderr, "ABORT: Failed creating template password file\n");
            return(255);
        }
    }

    if ( is_file(joinpath(tmpdir, "/group")) < 0 ) {
        if ( build_group(joinpath(containerpath, "/etc/group"), joinpath(tmpdir, "/group")) < 0 ) {
            fprintf(stderr, "ABORT: Failed creating template group file\n");
            return(255);
        }
    }


//****************************************************************************//
// Bind mounts                                                                //
//****************************************************************************//

    if ( seteuid(0) < 0 ) {
        fprintf(stderr, "ABORT: Could not re-escalate effective user privledges!\n");
        return(255);
    }

    if ( is_dir(joinpath(containerpath, "/dev/")) == 0 ) {
        if ( mount_bind("/dev", joinpath(containerpath, "/dev"), 0) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind mount /dev\n");
            return(255);
        }
    }

    if (is_file(joinpath(containerpath, "/etc/resolv.conf")) == 0 ) {
        if ( mount_bind("/etc/resolv.conf", joinpath(containerpath, "/etc/resolv.conf"), 0) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind /etc/resolv.conf\n");
            return(255);
        }
    }

    if (is_file(joinpath(containerpath, "/etc/hosts")) == 0 ) {
        if ( mount_bind("/etc/hosts", joinpath(containerpath, "/etc/hosts"), 0) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind /etc/hosts\n");
            return(255);
        }
    }

    if (is_file(joinpath(containerpath, "/etc/passwd")) == 0 ) {
        if ( mount_bind(joinpath(tmpdir, "/passwd"), joinpath(containerpath, "/etc/passwd"), 0) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind /etc/passwd\n");
            return(255);
        }
    }

    if (is_file(joinpath(containerpath, "/etc/group")) == 0 ) {
        if ( mount_bind(joinpath(tmpdir, "/group"), joinpath(containerpath, "/etc/group"), 0) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind /etc/group\n");
            return(255);
        }
    }

    if (is_file(joinpath(containerpath, "/etc/nsswitch.conf")) == 0 ) {
        if ( is_file(joinpath(SYSCONFDIR, "/singularity/default-nsswitch.conf")) == 0 ) {
            if ( mount_bind(joinpath(SYSCONFDIR, "/singularity/default-nsswitch.conf"), joinpath(containerpath, "/etc/nsswitch.conf"), 0) < 0 ) {
                fprintf(stderr, "ABORT: Could not bind %s\n", joinpath(SYSCONFDIR, "/singularity/default-nsswitch.conf"));
                return(255);
            }
        } else {
            fprintf(stderr, "WARNING: Template /etc/nsswitch.conf does not exist: %s\n", joinpath(SYSCONFDIR, "/singularity/default-nsswitch.conf"));
        }
    }

    if ( getenv("SINGULARITY_CONTAIN") == NULL ) {
        unsetenv("SINGULARITY_CONTAIN");

        if ( is_dir(joinpath(containerpath, "/tmp")) == 0 ) {
            if ( mount_bind("/tmp", joinpath(containerpath, "/tmp"), 1) < 0 ) {
                fprintf(stderr, "ABORT: Could not bind mount /tmp\n");
                return(255);
            }
        }
        if ( is_dir(joinpath(containerpath, "/var/tmp")) == 0 ) {
            if ( mount_bind("/var/tmp", joinpath(containerpath, "/var/tmp"), 1) < 0 ) {
                fprintf(stderr, "ABORT: Could not bind mount /var/tmp\n");
                return(255);
            }
        }

        if ( is_dir(joinpath(containerpath, basehomepath)) == 0 ){
            if ( mount_bind(basehomepath, joinpath(containerpath, basehomepath), 1) < 0 ) {
                fprintf(stderr, "ABORT: Could not bind home path to container %s: %s\n", homepath, strerror(errno));
                return(255);
            }
        } else {
            fprintf(stderr, "WARNING: Directory not existant in container: %s\n", basehomepath);
        }

    } else {
        if ( mount_bind(joinpath(tmpdir, "/tmp"), joinpath(containerpath, "/tmp"), 1) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind tmp path to container %s: %s\n", "/tmp", strerror(errno));
            return(255);
        }
        if ( mount_bind(joinpath(tmpdir, "/tmp"), joinpath(containerpath, "/var/tmp"), 1) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind tmp path to container %s: %s\n", "/var/tmp", strerror(errno));
            return(255);
        }
        if ( mount_bind(joinpath(tmpdir, basehomepath), joinpath(containerpath, basehomepath), 1) < 0 ) {
            fprintf(stderr, "ABORT: Could not bind tmp path to container %s: %s\n", basehomepath, strerror(errno));
            return(255);
        }
        strcpy(cwd, homepath);
    }



//****************************************************************************//
// Fork child in new namespaces                                               //
//****************************************************************************//

    child_pid = fork();

    if ( child_pid == 0 ) {
        char *prompt;


//****************************************************************************//
// Enter the file system                                                      //
//****************************************************************************//

        if ( chroot(containerpath) < 0 ) {
            fprintf(stderr, "ABORT: failed enter CONTAINERIMAGE: %s\n", containerpath);
            return(255);
        }


//****************************************************************************//
// Setup real mounts within the container                                     //
//****************************************************************************//

        if ( is_dir("/proc") == 0 ) {
            if ( mount("proc", "/proc", "proc", 0, NULL) < 0 ) {
                fprintf(stderr, "ABORT: Could not mount /proc: %s\n", strerror(errno));
                return(255);
            }
        }
        if ( is_dir("/sys") == 0 ) {
            if ( mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 ) {
                fprintf(stderr, "ABORT: Could not mount /sys: %s\n", strerror(errno));
                return(255);
            }
        }


//****************************************************************************//
// Drop all privledges for good                                               //
//****************************************************************************//

        if ( setregid(gid, gid) < 0 ) {
            fprintf(stderr, "ABORT: Could not dump real and effective group privledges!\n");
            return(255);
        }
        if ( setreuid(uid, uid) < 0 ) {
            fprintf(stderr, "ABORT: Could not dump real and effective user privledges!\n");
            return(255);
        }


//****************************************************************************//
// Setup final envrionment                                                    //
//****************************************************************************//

        prompt = (char *) malloc(strlen(containername) + 16);
        snprintf(prompt, strlen(containerimage) + 16, "Singularity/%s> ", containername);
        setenv("PS1", prompt, 1);

        // After this, we exist only within the container... Let's make it known!
        if ( setenv("SINGULARITY_CONTAINER", "true", 0) != 0 ) {
            fprintf(stderr, "ABORT: Could not set SINGULARITY_CONTAINER to 'true'\n");
            return(1);
        }

        if ( is_dir(cwd) == 0 ) {
            if ( chdir(cwd) < 0 ) {
                fprintf(stderr, "ABORT: Could not chdir to: %s\n", cwd);
                return(1);
            }
        } else {
            if ( fchdir(cwd_fd) < 0 ) {
                fprintf(stderr, "ABORT: Could not fchdir to cwd\n");
                return(1);
            }
        }


//****************************************************************************//
// Execv to container process                                                 //
//****************************************************************************//

        if ( command == NULL ) {
            fprintf(stderr, "No command specified, launching 'shell'\n");
            argv[0] = strdup("/bin/sh");
            if ( execv("/bin/sh", argv) != 0 ) {
                fprintf(stderr, "ABORT: exec of /bin/sh failed: %s\n", strerror(errno));
            }

        } else if ( strcmp(command, "run") == 0 ) {
            if ( is_exec("/singularity") == 0 ) {
                argv[0] = strdup("/singularity");
                if ( execv("/singularity", argv) != 0 ) {
                    fprintf(stderr, "ABORT: exec of /bin/sh failed: %s\n", strerror(errno));
                }
            } else {
                fprintf(stderr, "No Singularity runscript found, launching 'shell'\n");
                argv[0] = strdup("/bin/sh");
                if ( execv("/bin/sh", argv) != 0 ) {
                    fprintf(stderr, "ABORT: exec of /bin/sh failed: %s\n", strerror(errno));
                }
            }

        } else if ( strcmp(command, "exec") == 0 ) {
            char *args[4];

            args[0] = strdup("Singularity");
            args[1] = strdup("-c");
            args[2] = strdup(argv[1]);
            args[3] = NULL;

            if ( execv("/bin/sh", args) != 0 ) {
                fprintf(stderr, "ABORT: exec of '%s' failed: %s\n", command_exec, strerror(errno));
            }

        } else if ( strcmp(command, "shell") == 0 ) {
            argv[0] = strdup("/bin/sh");
            if ( execv("/bin/sh", argv) != 0 ) {
                fprintf(stderr, "ABORT: exec of /bin/sh failed: %s\n", strerror(errno));
            }

        } else {
            fprintf(stderr, "ABORT: Unrecognized Singularity command: %s\n", command);
            return(1);
        }

        return(255);


//****************************************************************************//
// Parent process waits for child                                             //
//****************************************************************************//

    } else if ( child_pid > 0 ) {
        int tmpstatus;
        signal(SIGINT, sighandler);
        signal(SIGKILL, sighandler);
        signal(SIGQUIT, sighandler);

        waitpid(child_pid, &tmpstatus, 0);
        retval = WEXITSTATUS(tmpstatus);
    } else {
        fprintf(stderr, "ABORT: Could not fork child process\n");
        return(255);
    }


//****************************************************************************//
// Finall wrap up before exiting                                              //
//****************************************************************************//

    if ( close(cwd_fd) < 0 ) {
        fprintf(stderr, "ERROR: Could not close cwd_fd!\n");
        retval++;
    }

    if ( flock(tmpdirlock_fd, LOCK_EX | LOCK_NB) == 0 ) {
        close(tmpdirlock_fd);
        if ( s_rmdir(tmpdir) < 0 ) {
            fprintf(stderr, "WARNING: Could not remove all files in %s: %s\n", tmpdir, strerror(errno));
        }
    } else {
//        printf("Not removing tmpdir, lock still\n");
    }

    close(containerimage_fd);
    close(tmpdirlock_fd);

    free(lockfile);
    free(containerpath);
    free(tmpdir);

    return(retval);
}
