#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "container.hpp"
#include "device.hpp"
#include "config.hpp"
#include "network.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <grp.h>
#include <net/if.h>
}

std::list<std::string> IpcSysctls = {
    "fs.mqueue.queues_max",
    "fs.mqueue.msg_max",
    "fs.mqueue.msgsize_max",
    "fs.mqueue.msg_default",
    "fs.mqueue.msgsize_default",

    "kernel.shmmax",
    "kernel.shmall",
    "kernel.shmmni",
    "kernel.shm_rmid_forced",

    "kernel.msgmax",
    "kernel.msgmni",
    "kernel.msgmnb",

    "kernel.sem",
};

void InitIpcSysctl() {
    for (const auto &key: IpcSysctls) {
        bool set = false;
        for (const auto &it: config().container().ipc_sysctl())
            set |= it.key() == key;
        std::string val;
        /* load default ipc sysctl from host config */
        if (!set && !GetSysctl(key, val)) {
            auto sysctl = config().mutable_container()->add_ipc_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

void TTaskEnv::ReportPid(pid_t pid) {
    TError error = Sock.SendPid(pid);
    if (error && error.GetErrno() != ENOMEM) {
        L_ERR("{}", error);
        Abort(error);
    }
    ReportStage++;
}

void TTaskEnv::Abort(const TError &error) {
    TError error2;

    /*
     * stage0: RecvPid WPid
     * stage1: RecvPid VPid
     * stage2: RecvError
     */
    L("abort due to {}", error);

    for (int stage = ReportStage; stage < 2; stage++) {
        error2 = Sock.SendPid(getpid());
        if (error2 && error2.GetErrno() != ENOMEM)
            L_ERR("{}", error2);
    }

    error2 = Sock.SendError(error);
    if (error2 && error2.GetErrno() != ENOMEM)
        L_ERR("{}", error2);

    _exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    TTaskEnv *task = static_cast<TTaskEnv*>(arg);
    task->StartChild();
    return EXIT_FAILURE;
}

TError TTaskEnv::ChildExec() {

    /* set environment for wordexp */
    TError error = Env.Apply();

    auto envp = Env.Envp();

    if (CT->IsMeta()) {
        const char *args[] = {
            "portoinit",
            "--container",
            CT->Name.c_str(),
            NULL,
        };
        SetDieOnParentExit(0);
        TFile::CloseAll({PortoInit.Fd, Sock.GetFd(), LogFile.Fd});
        fexecve(PortoInit.Fd, (char *const *)args, envp);
        return TError(EError::InvalidValue, errno, "fexecve(" +
                      std::to_string(PortoInit.Fd) +  ", portoinit)");
    }

    wordexp_t result;

    int ret = wordexp(CT->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        return TError(EError::Unknown, EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        return TError(EError::Unknown, EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        return TError(EError::Unknown, EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        return TError(EError::Unknown, EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        return TError(EError::Unknown, EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    if (Verbose) {
        L("command={}", CT->Command);
        for (unsigned i = 0; result.we_wordv[i]; i++)
            L("argv[{}]={}", i, result.we_wordv[i]);
        for (unsigned i = 0; envp[i]; i++)
            L("environ[{}]={}", i, envp[i]);
    }
    SetDieOnParentExit(0);
    TFile::CloseAll({0, 1, 2, Sock.GetFd(), LogFile.Fd});
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, envp);

    return TError(EError::InvalidValue, errno, std::string("execvpe(") +
            result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ")");
}

TError TTaskEnv::ChildApplyLimits() {
    auto ulimit = CT->GetUlimit();

    for (const auto &it :ulimit) {
        struct rlimit lim;
        int res;
        TError error;

        error = ParseUlimit(it.first, it.second, res, lim);
        if (error)
            return error;

        int ret = setrlimit(res, &lim);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                          "setrlimit " + it.first + " " + it.second);
    }

    return TError::Success();
}

TError TTaskEnv::WriteResolvConf() {
    if (!CT->ResolvConf.size())
        return TError::Success();
    std::string cfg = StringReplaceAll(CT->ResolvConf, ";", "\n");
    return TPath("/etc/resolv.conf").WritePrivate(cfg);
}

TError TTaskEnv::SetHostname() {
    TError error;

    if (CT->Hostname.size()) {
        error = TPath("/etc/hostname").WritePrivate(CT->Hostname + "\n");
        if (!error)
            error = SetHostName(CT->Hostname);
    }

    return error;
}

TError TTaskEnv::ApplySysctl() {
    TError error;

    if (CT->Isolate) {
        for (const auto &it: config().container().ipc_sysctl()) {
            error = SetSysctl(it.key(), it.val());
            if (error)
                return error;
        }
    }

    for (const auto &it: CT->Sysctl) {
        auto &key = it.first;

        if (TNetwork::NetworkSysctl(key)) {
            if (!CT->NetIsolate)
                return TError(EError::Permission, "Sysctl " + key + " requires net isolation");
            continue; /* Set by TNetEnv */
        } else if (std::find(IpcSysctls.begin(), IpcSysctls.end(), key) != IpcSysctls.end()) {
            if (!CT->Isolate)
                return TError(EError::Permission, "Sysctl " + key + " requires ipc isolation");
        } else
            return TError(EError::Permission, "Sysctl " + key + " is not allowed");

        error = SetSysctl(key, it.second);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTaskEnv::ConfigureChild() {
    TError error;

    error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (NewMountNs) {
        error = Mnt.Setup();
        if (error)
            return error;
    }

    error = ApplySysctl();
    if (error)
        return error;

    if (NewMountNs) {
        error = Mnt.ProtectProc();
        if (error)
            return error;
    }

    for (auto &dev: Devices) {
        error = dev.Makedev();
        if (error)
            return error;
    }

    error = WriteResolvConf();
    if (error)
        return error;

    error = SetHostname();
    if (error)
        return error;

    error = Mnt.Cwd.Chdir();
    if (error)
        return error;

    if (QuadroFork) {
        pid_t pid = fork();
        if (pid < 0)
            return TError(EError::Unknown, errno, "fork()");

        if (pid) {
            auto pid_ = std::to_string(pid);
            const char * argv[] = {
                "portoinit",
                "--container",
                CT->Name.c_str(),
                "--wait",
                pid_.c_str(),
                NULL,
            };
            auto envp = Env.Envp();

            error = PortoInitCapabilities.ApplyLimit();
            if (error)
                return error;

            TFile::CloseAll({PortoInit.Fd, Sock.GetFd(), LogFile.Fd});
            fexecve(PortoInit.Fd, (char *const *)argv, envp);
            return TError(EError::Unknown, errno, "fexecve()");
        }

        if (setsid() < 0)
            return TError(EError::Unknown, errno, "setsid()");
    }

    /* Report VPid */
    if (TripleFork) {
        MasterSock2.Close();
        error = Sock2.SendPid(GetPid());
        if (error)
            return error;
        /* Wait VPid Ack */
        error = Sock2.RecvZero();
        if (error)
            return error;
        /* Parent forwards VPid */
        ReportStage++;
        Sock2.Close();
    } else
        ReportPid(GetPid());

    error = TPath("/proc/self/loginuid").WriteAll(std::to_string(LoginUid));
    if (error && error.GetErrno() != ENOENT)
        L_WRN("Cannot set loginuid: {}", error);

    error = Cred.Apply();
    if (error)
        return error;

    if (HasAmbientCapabilities)
        L("Ambient capabilities: {}", CT->CapAmbient);

    error = CT->CapAmbient.ApplyAmbient();
    if (error)
        return error;

    L("Capabilities: {}", CT->CapBound);

    error = CT->CapBound.ApplyLimit();
    if (error)
        return error;

    if (!Cred.IsRootUser()) {
        error = CT->CapAmbient.ApplyEffective();
        if (error)
            return error;
    }

    error = CT->Stdin.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stdout.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stderr.OpenInside(*CT);
    if (error)
        return error;

    umask(CT->Umask);

    return TError::Success();
}

TError TTaskEnv::WaitAutoconf() {
    if (Autoconf.empty())
        return TError::Success();

    SetProcessName("portod-autoconf");

    auto sock = std::make_shared<TNl>();
    TError error = sock->Connect();
    if (error)
        return error;

    for (auto &name: Autoconf) {
        TNlLink link(sock, name);

        error = link.Load();
        if (error)
            return error;

        error = link.WaitAddress(config().network().autoconf_timeout_s());
        if (error)
            return error;
    }

    return TError::Success();
}

void TTaskEnv::StartChild() {
    TError error;

    if (TripleFork) {
        /* Die together with parent who report WPid */
        SetDieOnParentExit(SIGKILL);
    } else {
        /* Report WPid */
        ReportPid(GetPid());
    }

    /* Wait WPid Ack */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Apply configuration */
    error = ConfigureChild();
    if (error)
        Abort(error);

    /* Wait for Wakeup */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Reset signals before exec, signal block already lifted */
    ResetIgnoredSignals();

    error = WaitAutoconf();
    if (error)
        Abort(error);

    error = ChildExec();
    Abort(error);
}

TError TTaskEnv::Start() {
    TError error, error2;

    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;

    error = TUnixSocket::SocketPair(MasterSock, Sock);
    if (error)
        return error;

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    TTask task;

    error = task.Fork();
    if (error) {
        Sock.Close();
        L("Can't spawn child: {}", error);
        return error;
    }

    if (!task.Pid) {
        TError error;

        /* Switch from signafd back to normal signal delivery */
        ResetBlockedSignals();

        SetDieOnParentExit(SIGKILL);

        SetProcessName("portod-CT" + std::to_string(CT->Id));

        /* FIXME try to replace clone() with  unshare() */
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
        char stack[8192*4];
#else
        char stack[8192];
#endif

        (void)setsid();

        // move to target cgroups
        for (auto &cg : Cgroups) {
            error = cg.Attach(GetPid());
            if (error)
                Abort(error);
        }

        error = TPath("/proc/self/oom_score_adj").WriteAll(std::to_string(CT->OomScoreAdj));
        if (error && CT->OomScoreAdj)
            Abort(error);

        if (setpriority(PRIO_PROCESS, 0, CT->SchedNice))
            Abort(TError(EError::Unknown, errno, "setpriority"));

        struct sched_param param;
        param.sched_priority = CT->SchedPrio;
        if (sched_setscheduler(0, CT->SchedPolicy, &param))
            Abort(TError(EError::Unknown, errno, "sched_setparm"));

        if (SetIoPrio(0, CT->IoPrio))
            Abort(TError(EError::Unknown, errno, "ioprio"));

        /* Default streams and redirections are outside */
        error = CT->Stdin.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stdout.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stderr.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        /* Enter namespaces */

        error = IpcFd.SetNs(CLONE_NEWIPC);
        if (error)
            Abort(error);

        error = UtsFd.SetNs(CLONE_NEWUTS);
        if (error)
            Abort(error);

        error = NetFd.SetNs(CLONE_NEWNET);
        if (error)
            Abort(error);

        error = PidFd.SetNs(CLONE_NEWPID);
        if (error)
            Abort(error);

        error = MntFd.SetNs(CLONE_NEWNS);
        if (error)
            Abort(error);

        error = RootFd.Chroot();
        if (error)
            Abort(error);

        error = CwdFd.Chdir();
        if (error)
            Abort(error);

        if (TripleFork) {
            /*
             * Enter into pid-namespace. fork() hangs in libc if child pid
             * collide with parent pid outside. vfork() has no such problem.
             */
            pid_t forkPid = vfork();
            if (forkPid < 0)
                Abort(TError(EError::Unknown, errno, "fork()"));

            if (forkPid)
                _exit(EXIT_SUCCESS);

            error = TUnixSocket::SocketPair(MasterSock2, Sock2);
            if (error)
                Abort(error);

            /* Report WPid */
            ReportPid(GetTid());
        }

        int cloneFlags = SIGCHLD;
        if (CT->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        /* Create UTS namspace if hostname is changed or isolate=true */
        if (CT->Isolate || CT->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);

        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            Abort(error);
        }

        if (!TripleFork)
            _exit(EXIT_SUCCESS);

        /* close other side before reading */
        Sock2.Close();

        pid_t appPid, appVPid;
        error = MasterSock2.RecvPid(appPid, appVPid);
        if (error)
            Abort(error);

        /* Forward VPid */
        ReportPid(appPid);

        /* Ack VPid */
        error = MasterSock2.SendZero();
        if (error)
            Abort(error);

        MasterSock2.Close();

        auto pid = std::to_string(clonePid);
        const char * argv[] = {
            "portoinit",
            "--container",
            CT->Name.c_str(),
            "--wait",
            pid.c_str(),
            NULL,
        };
        auto envp = Env.Envp();

        error = PortoInitCapabilities.ApplyLimit();
        if (error)
            _exit(EXIT_FAILURE);

        TFile::CloseAll({PortoInit.Fd});
        fexecve(PortoInit.Fd, (char *const *)argv, envp);
        kill(clonePid, SIGKILL);
        _exit(EXIT_FAILURE);
    }

    Sock.Close();

    error = MasterSock.SetRecvTimeout(config().container().start_timeout_ms());
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->WaitTask.Pid, CT->TaskVPid);
    if (error)
        goto kill_all;

    /* Ack WPid */
    error = MasterSock.SendZero();
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->Task.Pid, CT->TaskVPid);
    if (error)
        goto kill_all;

    error2 = task.Wait();

    /* Task was alive, even if it already died we'll get zombie */
    error = MasterSock.SendZero();
    if (error)
        L("Task wakeup error: {}", error);

    /* Prefer reported error if any */
    error = MasterSock.RecvError();
    if (error)
        goto kill_all;

    if (!error && error2) {
        error = error2;
        goto kill_all;
    }

    return TError::Success();

kill_all:
    L("Task start failed: {}", error);
    if (task.Pid) {
        task.Kill(SIGKILL);
        task.Wait();
    }
    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;
    return error;
}
