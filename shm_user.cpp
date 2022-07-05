#include "shm/shm_user.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>

#include "log/ld_log.h"
#include "time/date_time.h"

#ifndef _WINDOWS
///////////////////////////////// LINUX ///////////////////////////////////////

    #include <sys/sem.h>
    #include <sys/shm.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>

ShmUser::ShmUser(const std::string &name, uint64_t size) {
    shm_name_ = name;
    key_t key = ftok(name.c_str(), 1);

    if (key <= 0 || size <= 0) {
        printf("------error msg----- \n");
        printf("name = %s \n", name.c_str());
        printf("key = %d\n", key);
        // 32bit cpu // 64 bit cpu
        printf("size = %" PRIu64 "\n", size);
        printf("------error msg----- \n");
        EXPECTTRUE(key > 0 && size > 0);
    }
    Connect(key, size);
}

bool ShmUser::Connect(key_t key, uint64_t size) {
    if (key <= 0 || size <= 0) {
        printf("------error msg----- \n");
        printf("key = %d \n", key);
        // 32bit cpu // 64 bit cpu
        printf("size = %" PRIu64 "\n", size);
        printf("------error msg----- \n");
        EXPECTTRUE(key > 0 && size > 0);
    }

    // 找到了对应的KEY和SIZE
    shm_key_          = key;
    shm_size_         = size;
    shm_useable_size_ = shm_size_ - sizeof(ShmHead);
    shm_id_           = shmget(shm_key_, shm_size_, 0666 | IPC_CREAT);
    if (shm_id_ < 0) {
        // get shm error:Invalid argument error:22
        // 最简单办法，直接重启机器即可．从程序来讲，还是先删除，再产生;
        // https://blog.csdn.net/libaineu2004/article/details/76919711
        // 注意 WSL 系统下不会成功.
        FLOGE("get shm error:%s error:%d", strerror(errno), errno);
        return false;
    }
    void *shmaddr = shmat(shm_id_, NULL, 0);
    if (shmaddr == (void *)-1) {
        FLOGE("attach shm error:%s", strerror(errno));
        return false;
    }

    shared_ptr_   = (uint8_t *)shmaddr;
    sem_id_       = semget(shm_key_, 1, 0666 | IPC_CREAT);
    is_connected_ = true;
    return true;
}

// ShmUser::ShmUser(key_t key, uint64_t size)
// {
//     if (key <= 0 || size <= 0) {
//         printf("------error msg----- \n");
//         printf("key = %d size = %lu \n", key, size);
//         printf("------error msg----- \n");
//         EXPECTTRUE(key > 0 && size > 0);
//     }
//     Connect(key, size);
// }

void ShmUser::DisConnect() {
    if (shared_ptr_ != nullptr) {
        shmdt(shared_ptr_);
        shared_ptr_   = nullptr;
        is_connected_ = false;
    }
}

void ShmUser::Lock() {
    sembuf sem_b;
    sem_b.sem_num = 0;
    sem_b.sem_op  = -1;
    sem_b.sem_flg = SEM_UNDO;

    int semRet = 0, retryCnt = 100, err = 0;

    // 4:Interrupted system call
    // 申请是阻塞操作，如果被中断就会返回用户态产生Interrupted system call错误
    // 需要重做P操作
    do {
        semRet = semop(sem_id_, &sem_b, 1);
        if (semRet == -1) {
            err = errno;
            std::string err_str;
            err_str = std::string(strerror(errno));
            FLOGW("semaphore_p lock failed!!! retryCnt:%d, errnoid:%d, errno:%s", retryCnt, err, err_str.c_str());
            --retryCnt;
            if (retryCnt == 0) {
                EXPECTTRUE(false);
            }
        }
    } while (semRet == -1 && err == 4 && retryCnt > 0);   // 4 EINTR
}

void ShmUser::Unlock() {
    sembuf sem_b;
    sem_b.sem_num = 0;
    sem_b.sem_op  = 1;
    sem_b.sem_flg = SEM_UNDO;

    int semRet = 0, retryCnt = 100, err = 0;

    do {
        semRet = semop(sem_id_, &sem_b, 1);
        if (semRet == -1) {
            err = errno;
            std::string err_str;
            err_str = std::string(strerror(errno));
            FLOGW("semaphore_p unlock failed!!! retryCnt:%d, errnoid:%d, errno:%s", retryCnt, err, err_str.c_str());
            --retryCnt;
            if (retryCnt == 0) {
                FLOGD("semaphore_p failed!! abort()");
                abort();
            }
        }
    } while (semRet == -1 && err == 4 && retryCnt > 0);   // 4 EINTR
}

#else   ///////////////////////////////// WINDOWS ///////////////////////////////////////

    #ifdef UNICODE
typedef LPCWSTR TSTR;
    #else
typedef LPCSTR TSTR;
    #endif

TSTR stringToTSTR(std::string &orig) {
    #ifdef UNICODE
    const char *c    = orig.c_str();
    int len          = MultiByteToWideChar(CP_ACP, 0, c, strlen(c), NULL, 0);
    wchar_t *m_wchar = new wchar_t[len + 1];
    MultiByteToWideChar(CP_ACP, 0, c, strlen(c), m_wchar, len);
    m_wchar[len] = '\0';
    return m_wchar;
    #else
    const char *c = orig.c_str();
    return c;
    #endif
}

ShmUser::ShmUser(const std::string &name, uint64_t size) {
    shm_name_ = name;
    // 后缀同
    // ShmBuilder::Create(const std::string& name, size_t size)
    std::string key = name + SHM_KEY_SUFFIX;
    Connect(name, key, size);
}

bool ShmUser::Connect(const std::string &name, const std::string &key, const uint64_t &size) {
    shm_key_          = key;
    shm_size_         = size;
    shm_useable_size_ = shm_size_ - sizeof(ShmHead);

    // cout << "shm_name:" << tempname << " key:" << tempkey << " mShmSize:" << mShmSize << endl;
    hMap        = ::OpenFileMapping(FILE_MAP_ALL_ACCESS, 0, (TSTR)name.c_str());
    shared_ptr_ = (uint8_t *)::MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);

    if (shared_ptr_ == nullptr) {
        // cout << "连接共享内存失败" << endl;

    } else {
        // cout << "连接共享内存成功" << endl;
    }

    hSem = CreateSemaphore(NULL, 1, 1, stringToTSTR(shm_key_));

    if (hSem == NULL) {
        printf("信号量获取失败\n");
        assert(false);
    } else {
        // cout << "信号量获取成功" << endl;
    }
    is_connected_ = true;
    return true;
}

void ShmUser::DisConnect() {
    if (shared_ptr_ != nullptr) {
        ::UnmapViewOfFile(shared_ptr_);
        ::CloseHandle(hMap);
        shared_ptr_   = nullptr;
        is_connected_ = false;
    }
}

void ShmUser::Lock() {
    // cout << "Locking Semaphore" << endl;
    WaitForSingleObject(hSem, INFINITE);
}

void ShmUser::Unlock() {
    // cout << "Unlock Semaphore" << endl;
    ReleaseSemaphore(hSem, 1, NULL);
}

#endif   // _WINDOWS ////////////////////////////////////////////////////////////////////////
uint32_t ShmUser::GetShmMaxLen(const std::string &name) {
    ShmUser user(name, sizeof(ShmHead));
    ShmHead header;
    user.GetLastChangeInfo(&header);
    return header.max_len;
}

ShmUser::ShmUser(const std::string &name) {
    uint32_t max_size = ShmUser::GetShmMaxLen(name);
    new (this) ShmUser(name, max_size);
}

ShmUser::~ShmUser() { DisConnect(); }

int ShmUser::Write(uint8_t *buf, const uint32_t &offset, const uint32_t &size) {
    if (buf == nullptr) {
        return -1;
    }
    EXPECTTRUE(shared_ptr_ != nullptr);
    Lock();
    int ret = UnsafeWrite(buf, offset, size);
    Unlock();
    return ret;
}

uint8_t *ShmUser::ShmPermissionAcquisition() {
    Lock();
    locked_ = true;
    return (uint8_t *)shared_ptr_ + sizeof(ShmHead);
}

void ShmUser::ShmPermissionRelease() {
    if (!locked_) {
        printf("%s,locked_ = %d \n", shm_name_.c_str(), locked_);
        EXPECTTRUE(locked_);
    }
    locked_ = false;
    Unlock();
}

void ShmUser::ShmPermissionRelease(const uint32_t &valid_len) {
    ShmHead shm_header;
    if (!locked_ || valid_len > shm_useable_size_) {
        printf("%s,locked_ = %d valid_len = %u shm_useable_size_ = %zu \n", shm_name_.c_str(), locked_, valid_len,
               shm_useable_size_);
        EXPECTTRUE(locked_ && valid_len < shm_useable_size_);
    }
    shm_header.len                 = valid_len;
    shm_header.max_len             = shm_size_;
    shm_header.last_write_pid      = getpid();
    shm_header.last_update_time_ms = GetCurrentTickMs();
    memcpy(shared_ptr_, &shm_header, sizeof(ShmHead));
    locked_ = false;
    Unlock();
}

int ShmUser::UnsafeWrite(uint8_t *buf, const uint32_t &offset, const uint32_t &size) {
    if (buf == nullptr) {
        return -1;
    }
    // 判断写入数据长度
    if (offset + size > shm_useable_size_) {
        printf("err msg:shm_name: %s offset:%d + %d  > shm_useable_size_:%zu----- \n", shm_name_.c_str(), offset, size,
               shm_useable_size_);
        EXPECTTRUE(false);
        return -1;
    } else {
        ShmHead shm_header;
        memcpy((uint8_t *)shared_ptr_ + sizeof(ShmHead) + offset, buf, size);
        shm_header.len                 = offset + size;
        shm_header.max_len             = shm_size_;
        shm_header.last_write_pid      = getpid();
        shm_header.last_update_time_ms = GetCurrentTickMs();
        memcpy(shared_ptr_, &shm_header, sizeof(ShmHead));
    }
    return 1;
}

// only for stable and enough length buffer
int ShmUser::Read(uint8_t *buf, const uint32_t &offset, const uint32_t &size) {
    if (buf == nullptr) {
        return -1;
    }
    EXPECTTRUE(shared_ptr_ != nullptr);
    Lock();
    int retLen = UnsafeRead(buf, offset, size);
    Unlock();
    return retLen;
}

void ShmUser::ClearAllData() {
    EXPECTTRUE(shared_ptr_ != nullptr);
    ShmHead shm_header;
    Lock();
    shm_header.len                 = 0;
    shm_header.max_len             = shm_size_;
    shm_header.last_write_pid      = getpid();
    shm_header.last_update_time_ms = GetCurrentTickMs();
    memcpy(shared_ptr_, &shm_header, sizeof(ShmHead));

    memset(shared_ptr_ + sizeof(ShmHead), 0, shm_useable_size_);

    Unlock();
}

void ShmUser::GetLastChangeInfo(ShmHead *shm_header) {
    if (shm_header == nullptr) {
        return;
    }
    if (shared_ptr_ == nullptr) {
        FLOGE("error: name = %s, shared_ptr_ = null ", shm_name_.c_str());
        EXPECTTRUE(shared_ptr_ != nullptr);
    }
    Lock();
    memcpy(shm_header, shared_ptr_, sizeof(ShmHead));
    Unlock();
}

uint32_t ShmUser::GetLen() {
    EXPECTTRUE(shared_ptr_ != nullptr);
    ShmHead shm_header;
    Lock();
    memcpy(&shm_header, shared_ptr_, sizeof(ShmHead));
    Unlock();
    return uint32_t(shm_header.len);
}

ShmHead ShmUser::UnsafeGetShmHead() {
    EXPECTTRUE(shared_ptr_ != nullptr);
    ShmHead shm_header;
    memcpy(&shm_header, shared_ptr_, sizeof(ShmHead));
    return shm_header;
}

int ShmUser::UnsafeRead(uint8_t *buf, const uint32_t &offset, const uint32_t &size) {
    EXPECTTRUE(shared_ptr_ != nullptr);
    ShmHead shm_header = UnsafeGetShmHead();
    int retLen         = size;
    uint8_t *p         = (uint8_t *)shared_ptr_ + sizeof(ShmHead) + offset;

    if (shm_header.max_len < (offset + size)) {
        FLOGE("shm_header.max_len < (offset + size)");
        retLen = 0;   // do nothing
    } else {
        memcpy(buf, p, size);
    }
    return retLen;
}

int ShmUser::InitFifo(const uint32_t &buffer_size) {
    EXPECTTRUE(shared_ptr_ != nullptr);
    Lock();
    ShmFifoHead fifo_head_init;
    fifo_head_init.header           = 0;
    fifo_head_init.fifo_buffer_size = buffer_size;
    int ret                         = UnsafeWrite((uint8_t *)&fifo_head_init, 0, sizeof(ShmFifoHead));
    Unlock();
    return ret;
}

int ShmUser::PushFifo(uint8_t *data_buff, const uint32_t &len) {
    if (data_buff == nullptr) {
        return -1;
    }
    EXPECTTRUE(shared_ptr_ != nullptr);
    Lock();
    ShmHead shm_header = UnsafeGetShmHead();
    if (shm_header.max_len < (sizeof(ShmFifoHead) + len)) {
        FLOGE("shm_header.max_len < (sizeof(ShmFifoHead) + len))");
        return -1;   // do nothing
    }

    uint8_t *p             = (uint8_t *)shared_ptr_ + sizeof(ShmHead);
    ShmFifoHead *fifo_head = (ShmFifoHead *)p;

    if (len > fifo_head->fifo_buffer_size) {
        return -1;
    }

    if (fifo_head->header + len > fifo_head->fifo_buffer_size) {
        memcpy(p + sizeof(ShmFifoHead) + fifo_head->header, data_buff, fifo_head->fifo_buffer_size - fifo_head->header);
        memcpy(p + sizeof(ShmFifoHead), data_buff + (fifo_head->fifo_buffer_size - fifo_head->header),
               len - (fifo_head->fifo_buffer_size - fifo_head->header));
        fifo_head->header = len - (fifo_head->fifo_buffer_size - fifo_head->header);
    } else {
        memcpy(p + sizeof(ShmFifoHead) + fifo_head->header, data_buff, len);
        fifo_head->header = len + fifo_head->header;
    }

    shm_header.len                 = fifo_head->fifo_buffer_size;
    shm_header.max_len             = shm_size_;
    shm_header.last_write_pid      = getpid();
    shm_header.last_update_time_ms = GetCurrentTickMs();
    memcpy(shared_ptr_, &shm_header, sizeof(ShmHead));
    Unlock();
    return 0;
}
