#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static pid_t g_pid = 0;

#define SYS_svc_stalker_ctl         8
#define PID_MANAGE                  0
#define CALL_LIST_MANAGE            1

static void interrupt(int sig){
    /* unregister this PID upon exit */
    syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
    write(1, "\nExiting\n", 9);
    exit(0);
}

extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

kern_return_t catch_mach_exception_raise_state(mach_port_t exception_port,
        exception_type_t exception, exception_data_t code,
        mach_msg_type_number_t code_count, int *flavor,
        thread_state_t in_state, mach_msg_type_number_t in_state_count,
        thread_state_t out_state, mach_msg_type_number_t *out_state_count){
    /* unused */
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t exception_port,
        mach_port_t thread, mach_port_t task, exception_type_t exception,
        exception_data_t code, mach_msg_type_number_t code_count, int *flavor,
        thread_state_t in_state, mach_msg_type_number_t in_state_count,
        thread_state_t out_state, mach_msg_type_number_t *out_state_count){
    /* unused */
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise(mach_port_t exception_port,
        mach_port_t thread, mach_port_t task, exception_type_t exception,
        mach_exception_data_t code, mach_msg_type_number_t code_count){
    /* If we're here, the system call/Mach trap has not yet happened.
     * Once we return from this function, the kernel carries it out as normal.
     * You're free to modify registers before giving control back to
     * the kernel.
     */

    /* you can also get the call number from X16 */
    long call_num = code[0];
    pid_t pid = code[1];

    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    arm_thread_state64_t state = {0};
    kern_return_t kret = thread_get_state(thread, ARM_THREAD_STATE64,
                (thread_state_t)&state, &count);

    if(kret){
        printf("thread_get_state failed: %s\n", mach_error_string(kret));
        return KERN_SUCCESS;
    }

    printf("%d: ", pid);

    /* write */
    if(call_num == 4){
        /* write(fd, buf, count)
         *
         * W0 == fd
         * X1 == buf
         * X2 == count
         */

        char buf[state.__x[2]];

        mach_msg_type_number_t sz = state.__x[2];
        kret = vm_read_overwrite(task, state.__x[1], sz, buf, &sz);

        if(kret){
            printf("vm_read_overwrite failed: %s\n", mach_error_string(kret));
            return KERN_SUCCESS;
        }

        printf("write(%lld, \"%s\", %lld)\n", state.__x[0], buf, state.__x[2]);
    }
    /* open or access */
    else if(call_num == 5 || call_num == 33){
        /* open(pathname, flags) or access(pathname, mode)
         * 
         * We can handle these both here because their parameters are
         * the exact same type
         *
         * X0 == pathname
         * W1 == flags/mode
         */
        char buf[PATH_MAX] = {0};

        mach_msg_type_number_t sz = PATH_MAX;
        kret = vm_read_overwrite(task, state.__x[0], sz, buf, &sz);

        if(kret){
            printf("vm_read_overwrite failed: %s\n", mach_error_string(kret));
            return KERN_SUCCESS;
        }

        if(call_num == 5)
            printf("open");
        else
            printf("access");

        printf("(\"%s\", %#x)\n", buf, (uint32_t)state.__x[1]);
    }
    /* mach_msg_trap */
    else if(call_num == -31){
        printf("mach_msg(%#llx, %#x, %#x, %#x, %#x, %#x, %#x)\n", state.__x[0],
                (uint32_t)state.__x[1], (uint32_t)state.__x[2],
                (uint32_t)state.__x[3], (uint32_t)state.__x[4],
                (uint32_t)state.__x[5], (uint32_t)state.__x[6]);
    }
    /* _kernelrpc_mach_port_allocate_trap */
    else if(call_num == -16){
        printf("_kernelrpc_mach_port_allocate_trap(%#x, %#x, %#llx)\n",
                (uint32_t)state.__x[0], (uint32_t)state.__x[1], state.__x[2]);
    }

    /* always return KERN_SUCCESS to let the kernel know you've handled
     * this exception
     */
    return KERN_SUCCESS;
}

static void *e_thread_func(void *arg){
    mach_port_t eport = (mach_port_t)arg;

    for(;;)
        mach_msg_server_once(mach_exc_server, 4096, eport, 0);

    return NULL;
}

int main(int argc, char **argv){
    if(argc < 2){
        printf("No PID\n");
        return 1;
    }

    /* first, was svc_stalker_ctl patched correctly? For all my phones, the patched
     * system call is always number 8. It could be different for you.
     */
    int ret = syscall(SYS_svc_stalker_ctl, -1, PID_MANAGE, 0, 0);

    if(ret != 999){
        printf("svc_stalker_ctl wasn't patched correctly\n");
        return 1;
    }

    /* install signal handler for Ctrl-C so when user wants to exit this
     * program, we also unregister the PID we're intercepting calls for
     */
    signal(SIGINT, interrupt);

    g_pid = atoi(argv[1]);

    mach_port_t tfp = MACH_PORT_NULL;
    kern_return_t kret = task_for_pid(mach_task_self(), g_pid, &tfp);

    if(kret){
        printf("task_for_pid for pid %d failed: %s\n", g_pid, mach_error_string(kret));
        return 1;
    }

    mach_port_t eport = MACH_PORT_NULL;

    kret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &eport);
    
    if(kret){
        printf("mach_port_allocate failed: %s\n", mach_error_string(kret));
        return 1;
    }

    kret = mach_port_insert_right(mach_task_self(), eport, eport,
            MACH_MSG_TYPE_MAKE_SEND);

    if(kret){
        printf("mach_port_insert_right failed: %s\n", mach_error_string(kret));
        return 1;
    }

    /* You *always* need to register exception ports for a process before registering
     * its PID with svc_stalker_ctl.
     *
     * System call/Mach trap interception exception messages will be sent to
     * you as EXC_SYSCALL or EXC_MACH_SYSCALL exceptions, so only filter for
     * those. After the kernel patches svc_stalker does, we are the only
     * thing which uses these types of exceptions.
     */
    kret = task_set_exception_ports(tfp, EXC_MASK_SYSCALL | EXC_MASK_MACH_SYSCALL,
            eport, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);

    if(kret){
        printf("task_set_exception_ports failed: %s\n", mach_error_string(kret));
        return 1;
    }

    /* That's all set up, so start filtering for write, open, access,
     * and _kernelrpc_mach_port_allocate_trap.
     *
     * register this PID for interception
     */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 1, 0);

    if(ret){
        printf("Failed registering %d for call interception: %s\n", g_pid,
                strerror(errno));
        return 1;
    }

    /* register some call numbers to intercept */
    /* write */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, 4, 1);

    if(ret){
        printf("Couldn't register write: %s\n", strerror(errno));
        /* always unregister */
        syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
        return 1;
    }

    /* open */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, 5, 1);

    if(ret){
        printf("Couldn't register open: %s\n", strerror(errno));
        /* always unregister */
        syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
        return 1;
    }

    /* access */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, 33, 1);

    if(ret){
        printf("Couldn't register access: %s\n", strerror(errno));
        /* always unregister */
        syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
        return 1;
    }

    /* mach_msg_trap */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, -31, 1);

    if(ret){
        printf("Couldn't register mach_msg_trap: %s\n", strerror(errno));
        /* always unregister */
        syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
        return 1;
    }

    /* _kernelrpc_mach_port_allocate_trap */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, -16, 1);

    if(ret){
        printf("Couldn't register mach_msg_trap: %s\n", strerror(errno));
        /* always unregister */
        syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
        return 1;
    }

    pthread_t e_thread;
    pthread_create(&e_thread, NULL, e_thread_func, (void *)(uintptr_t)eport);
    pthread_join(e_thread, NULL);

    /* not reached */

    return 0;
}
