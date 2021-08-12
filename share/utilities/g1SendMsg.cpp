#define HMALLOC_MSG_IMPL
#include "g1SendMsg.hpp"

messageAll msg;

messageAll::messageAll(){
            hmsg_start_sender(&sender);
            pid = getpid();
            hmalloc_msg msg;
            msg.header.msg_type = HMSG_INIT;
            msg.header.pid = pid;
            msg.init.mode = HMSG_MODE_OBJECT;
            hmsg_send(&sender, &msg);
            
            msg.header.msg_type = HMSG_LABL;
            msg.labl.hid = 9;
            snprintf(msg.labl.label, 32, "Young");
            
            hmsg_send(&sender, &msg);

            msg.header.msg_type = HMSG_LABL;
            msg.labl.hid = 5;
            snprintf(msg.labl.label, 32, "Archive");
            
            hmsg_send(&sender, &msg);

            msg.header.msg_type = HMSG_LABL;
            msg.labl.hid = 2;
            snprintf(msg.labl.label, 32, "Survivor");
            
            hmsg_send(&sender, &msg);

            msg.header.msg_type = HMSG_LABL;
            msg.labl.hid = 1;
            snprintf(msg.labl.label, 32, "Old");
            
            hmsg_send(&sender, &msg);
}

messageAll::~messageAll(){
            hmalloc_msg msg;
            msg.header.msg_type = HMSG_FINI;
            msg.header.pid = pid;
            hmsg_send(&sender, &msg);
            hmsg_close_sender(&sender);
        }
void messageAll::sendmsg(uint64_t addr, uint64_t size, uint32_t hid, int m){
        if(m == 0){
            hmalloc_msg msg;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            msg.header.msg_type = HMSG_ALLO;
            msg.header.pid = pid;
            msg.allo.addr = addr;
            msg.allo.size = size;
            msg.allo.timestamp_ns = 1000000000ULL * ts.tv_sec + ts.tv_nsec;
            msg.allo.hid = hid;
            hmsg_send(&sender, &msg);
        }
        if(m == 1){
            hmalloc_msg msg;
            msg.header.msg_type = HMSG_FREE;
            msg.header.pid = pid;
            msg.free.addr = addr;
            hmsg_send(&sender, &msg);
        }
        
}

void messageAll::updatemsg(uint64_t addr, uint32_t hid){
        hmalloc_msg msg;
        msg.header.msg_type = HMSG_UPDT;
        msg.header.pid = pid;
        msg.updt.addr = addr;
        msg.updt.hid = hid;
        hmsg_send(&sender, &msg);
}
