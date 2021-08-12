
#define HMALLOC_MSG_SENDER
#include "utilities/hmalloc_msg.h"
#include <time.h>

#ifndef SEND_MSG_H
#define SEND_MSG_H


class messageAll{
    pid_t pid;
    hmalloc_msg_sender sender;
    public:
        messageAll();
        ~messageAll();
        void sendmsg(uint64_t, uint64_t, uint32_t, int);
        void updatemsg(uint64_t, uint32_t);
};

extern messageAll msg;

#endif
