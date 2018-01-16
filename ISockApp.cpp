//
// Created by System Administrator on 12/26/17.
//

#include <cstdlib>
#include <ctime>

#include <string>

#include "uv.h"

#include "ISockApp.h"
#include "IConn.h"
#include "IRawConn.h"
#include "plog/Log.h"
#include "plog/Appenders/ConsoleAppender.h"
#include "util/FdUtil.h"
#include "tcp/SockMon.h"
#include "util/ProcUtil.h"

ISockApp::ISockApp(bool is_server, uv_loop_t *loop) : mServer(is_server) {
    mLoop = loop;
}

int ISockApp::Init(RConfig &conf) {
    if (!conf.Inited()) {
        fprintf(stderr, "conf must be inited\n");
#ifndef NNDEBUG
        assert(0);
#endif
        return -1;
    }
    mConf = conf;
    return Init();
}


int ISockApp::Init(const std::string &json_content) {
    std::string err;
    mConf.ParseJsonString(mConf, json_content, err);
    if (err.empty()) {
        return Init();
    }
    return -1;
}


int ISockApp::Parse(int argc, const char *const *argv) {
    assert(argv != nullptr);
    int nret = mConf.Parse(mServer, argc, argv);
    return nret;
}

int ISockApp::Init() {
    if (!mConf.Inited()) {
        fprintf(stderr, "configuration not inited.\n");
#ifndef NNDEBUG
        assert(0);
#else
        return -1;
#endif
    }
    return doInit();
}

int ISockApp::doInit() {
    assert(mConf.Inited());

    int nret = initLog();
    if (nret) {
        fprintf(stderr, "failed to init logger, nret: %d\n", nret);
        return -1;
    }

    if (makeDaemon(mConf.isDaemon)) {
        return -1;
    }

    mTimer = new RTimer(mLoop);
    LOGD << "conf: " << mConf.to_json().dump();
    mCap = CreateCap(mConf);
    if (!mCap || mCap->Init()) {
        return -1;
    }

    char err[LIBNET_ERRBUF_SIZE] = {0};
    mLibnet = libnet_init(LIBNET_RAW4, mConf.param.dev.c_str(), err);
    if (nullptr == mLibnet) {
        LOGE << "failed to init libnet: " << err;
        return -1;
    }

    mBtmConn = CreateBtmConn(mConf, mLoop, mCap->Datalink(), mConf.param.type);
    if (!mBtmConn) {
        return -1;
    }

    mMon = InitSockMon(mLoop, mConf);
    if (!mMon) {
        LOGE << "failed to create sockmon or init failed";
        return -1;
    }

    mBridge = CreateBridgeConn(mConf, mBtmConn, mLoop, mMon);

    if (!mBridge || mBridge->Init()) {
        return -1;
    }


    mInited = true;
    srand(time(NULL));
    return 0;
}

int ISockApp::initLog() {
    if (!mConf.log_path.empty()) {
        if (!FdUtil::FileExists(mConf.log_path.c_str())) {
            int nret = FdUtil::CreateFile(mConf.log_path);
            if (nret < 0) {
                return nret;
            }
        }
        mFileAppender = new plog::RollingFileAppender<plog::TxtFormatter>(mConf.log_path.c_str(), 100000, 5);
    } else  {
        fprintf(stderr, "warning: log path empty\n");
    }

    mConsoleAppender = new plog::ConsoleAppender<plog::TxtFormatter>();
    plog::init(mConf.log_level, mConsoleAppender);
    if (mFileAppender) {
        plog::get()->addAppender(mFileAppender);
    }

    return 0;
}
int ISockApp::Start() {
    assert(mInited);
    mCap->Start(IRawConn::CapInputCb, reinterpret_cast<u_char *>(mBtmConn)); // starts the thread
    StartTimer(mConf.param.interval * 1000 * 2, mConf.param.interval * 1000);

    return uv_run(mLoop, UV_RUN_DEFAULT);
}

void ISockApp::Flush(void *arg) {
    mBridge->CheckAndClose();
}

void ISockApp::Close() {
    if (mTimer) {
        mTimer->Stop();
        delete mTimer;
        mTimer = nullptr;
    }

    if (mCap) {
        mCap->Close();
        delete mCap;
        mCap = nullptr;
    }

    if (mBridge) {
        mBridge->Close();
        delete mBridge;
        mBridge = nullptr;
        mBtmConn = nullptr;
    }

    if (mLoop) {
        uv_stop(mLoop);
        uv_loop_delete(mLoop);
        mLoop = nullptr;
    }

    if (mMon) {
        mMon->Close();
        delete mMon;
        mMon = nullptr;
    }
}

void ISockApp::StartTimer(IUINT32 timeout_ms, IUINT32 repeat_ms) {
    auto fn = std::bind(&ISockApp::Flush, this, std::placeholders::_1);
    mTimer->Start(timeout_ms, repeat_ms, fn);
}

int ISockApp::makeDaemon(bool d) {
    int n = ProcUtil::MakeDaemon(d);
    if (n < 0) {
        LOGE << "make process daemon failed: " << strerror(errno);
        return n;
    }
    if (mConf.isDaemon) {   // todo: if run in daemon, poll will fail if use default loop (on mac, it's uv__io_check_fd fails). why?
        LOGI << "Run in background. pid: " << getpid(); // print to file.
        mLoop = static_cast<uv_loop_t *>(malloc(sizeof(uv_loop_t)));
        memset(mLoop, 0, sizeof(uv_loop_t));
        uv_loop_init(mLoop);
    }
    return 0;
}
