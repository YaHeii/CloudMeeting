#ifndef NETHEADER_H
#define NETHEADER_H
#include <QMetatype>
#include <QMutexLocker>
#include <QMutex>
#include <QWaitCondition>
#include <QByteArray>

#define QUEUE_MAXSIZE 1500

#ifndef MB
#define MB 1024*1024
#endif

#ifndef KB
#define KB 1024
#endif

#ifndef WAITSECONDS
#define WAITSECONDS 2
#endif

#ifndef OPENVIDEO
#define OPENVIDEO "打开视频"
#endif

#ifndef CLOSEVIDEO
#define CLOSEVIDEO "关闭视频"
#endif

#ifndef OPENAUDIO
#define OPENAUDIO "打开音频"
#endif

#ifndef CLOSEAUDIO
#define CLOSEAUDIO "关闭音频"
#endif


#ifndef MSG_HEADER
#define MSG_HEADER 11
#endif


enum MSG_TYPE {
    IMG_SEND = 0,
    IMG_RECV,
    AUDIO_SEND,
    AUDIO_RECV,
    TEXT_SEND,
    TEXT_RECV,
    CREATE_MEETING,
    EXIT_MEETING,
    JOIN_MEETING,
    CLOSE_CAMERA,

    CREATE_MEETING_RESPONSE = 20,
    PARTNER_EXIT = 21,
    PARTNER_JOIN = 22,
    JOIN_MEETING_RESPONSE = 23,
    PARTNER_JOIN2 = 24,
    RemoteHostClosedError = 40,
    OtherNetError = 41
};

Q_DECLARE_METATYPE(MSG_TYPE); //添加进入Qvariant
struct MESG {
    MSG_TYPE msg_type;
    uchar *data;
    long len;
    quint32 ip;
};


#endif // NETHEADER_H
