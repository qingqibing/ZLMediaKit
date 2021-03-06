//
// Created by xzl on 2018/8/30.
//

#include "Util/File.h"
#include "FlvMuxer.h"
#include "utils.h"

#define FILE_BUF_SIZE (64 * 1024)

namespace ZL {
namespace Rtmp {


FlvMuxer::FlvMuxer() {
}
FlvMuxer::~FlvMuxer() {
}

void FlvMuxer::start(const RtmpMediaSource::Ptr &media) {
    if(!media){
        throw std::runtime_error("RtmpMediaSource 无效");
    }

    onWriteFlvHeader(media);

    std::weak_ptr<FlvMuxer> weakSelf = getSharedPtr();
    _ring_reader = media->getRing()->attach();
    _ring_reader->setDetachCB([weakSelf](){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        strongSelf->onDetach();
    });
    _ring_reader->setReadCB([weakSelf](const RtmpPacket::Ptr &pkt){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        strongSelf->onWriteRtmp(pkt);
    });
}

void FlvMuxer::onWriteFlvHeader(const RtmpMediaSource::Ptr &mediaSrc) {
    m_previousTagSize = 0;
    CLEAR_ARR(m_aui32FirstStamp);

    //发送flv文件头
    char flv_file_header[] = "FLV\x1\x5\x0\x0\x0\x9"; // have audio and have video
    bool is_have_audio = false,is_have_video = false;

    mediaSrc->getConfigFrame([&](const RtmpPacket::Ptr &pkt){
        if(pkt->typeId == MSG_VIDEO){
            is_have_video = true;
        }
        if(pkt->typeId == MSG_AUDIO){
            is_have_audio = true;
        }
    });

    if (is_have_audio && is_have_video) {
        flv_file_header[4] = 0x05;
    } else if (is_have_audio && !is_have_video) {
        flv_file_header[4] = 0x04;
    } else if (!is_have_audio && is_have_video) {
        flv_file_header[4] = 0x01;
    } else {
        flv_file_header[4] = 0x00;
    }

    //flv header
    onWrite(flv_file_header, sizeof(flv_file_header) - 1);
    //metadata
    AMFEncoder invoke;
    invoke << "onMetaData" << mediaSrc->getMetaData();
    onWriteFlvTag(MSG_DATA, invoke.data(), 0);
    //config frame
    mediaSrc->getConfigFrame([&](const RtmpPacket::Ptr &pkt){
        onWriteRtmp(pkt);
    });
}



#if defined(_WIN32)
#pragma pack(push, 1)
#endif // defined(_WIN32)

class RtmpTagHeader {
public:
    uint8_t type = 0;
    uint8_t data_size[3] = {0};
    uint8_t timestamp[3] = {0};
    uint8_t timestamp_ex = 0;
    uint8_t streamid[3] = {0}; /* Always 0. */
}PACKED;

#if defined(_WIN32)
#pragma pack(pop)
#endif // defined(_WIN32)

class BufferRtmp : public Buffer{
public:
    typedef std::shared_ptr<BufferRtmp> Ptr;
    BufferRtmp(const RtmpPacket::Ptr & pkt):_rtmp(pkt){}
    virtual ~BufferRtmp(){}

    char *data() override {
        return (char *)_rtmp->strBuf.data();
    }
    uint32_t size() const override {
        return _rtmp->strBuf.size();
    }
private:
    RtmpPacket::Ptr _rtmp;
};


void FlvMuxer::onWriteFlvTag(const RtmpPacket::Ptr &pkt, uint32_t ui32TimeStamp) {
    auto size = htonl(m_previousTagSize);
    onWrite((char *)&size,4);//onWrite PreviousTagSize
    RtmpTagHeader header;
    header.type = pkt->typeId;
    set_be24(header.data_size, pkt->strBuf.size());
    header.timestamp_ex = (uint8_t) ((ui32TimeStamp >> 24) & 0xff);
    set_be24(header.timestamp,ui32TimeStamp & 0xFFFFFF);
    onWrite((char *)&header, sizeof(header));//onWrite tag header
    onWrite(std::make_shared<BufferRtmp>(pkt));//onWrite tag data
    m_previousTagSize += (pkt->strBuf.size() + sizeof(header));
}

void FlvMuxer::onWriteFlvTag(uint8_t ui8Type, const std::string &strBuf, uint32_t ui32TimeStamp) {
    auto size = htonl(m_previousTagSize);
    onWrite((char *)&size,4);//onWrite PreviousTagSize
    RtmpTagHeader header;
    header.type = ui8Type;
    set_be24(header.data_size, strBuf.size());
    header.timestamp_ex = (uint8_t) ((ui32TimeStamp >> 24) & 0xff);
    set_be24(header.timestamp,ui32TimeStamp & 0xFFFFFF);
    onWrite((char *)&header, sizeof(header));//onWrite tag header
    onWrite(std::make_shared<BufferString>(strBuf));//onWrite tag data
    m_previousTagSize += (strBuf.size() + sizeof(header));
}

void FlvMuxer::onWriteRtmp(const RtmpPacket::Ptr &pkt) {
    auto modifiedStamp = pkt->timeStamp;
    auto &firstStamp = m_aui32FirstStamp[pkt->typeId % 2];
    if(!firstStamp){
        firstStamp = modifiedStamp;
    }
    if(modifiedStamp >= firstStamp){
        //计算时间戳增量
        modifiedStamp -= firstStamp;
    }else{
        //发生回环，重新计算时间戳增量
        CLEAR_ARR(m_aui32FirstStamp);
        modifiedStamp = 0;
    }
    onWriteFlvTag(pkt, modifiedStamp);
}

void FlvMuxer::stop() {
    if(_ring_reader){
        _ring_reader.reset();
        onDetach();
    }
}

///////////////////////////////////////////////////////FlvRecorder/////////////////////////////////////////////////////
void FlvRecorder::startRecord(const string &vhost, const string &app, const string &stream,const string &file_path) {
    startRecord(dynamic_pointer_cast<RtmpMediaSource>(MediaSource::find(RTMP_SCHEMA,vhost,app,stream,false)),file_path);
}

void FlvRecorder::startRecord(const RtmpMediaSource::Ptr &media, const string &file_path) {
    stop();
    lock_guard<recursive_mutex> lck(_file_mtx);
    //开辟文件写缓存
    std::shared_ptr<char> fileBuf(new char[FILE_BUF_SIZE],[](char *ptr){
        if(ptr){
            delete [] ptr;
        }
    });
    //新建文件
    _file.reset(File::createfile_file(file_path.data(),"wb"),[fileBuf](FILE *fp){
        if(fp){
            fflush(fp);
            fclose(fp);
        }
    });
    if (!_file){
        throw std::runtime_error( StrPrinter << "打开文件失败:" << file_path);
    }

    //设置文件写缓存
    setvbuf( _file.get(), fileBuf.get(),_IOFBF, FILE_BUF_SIZE);
    start(media);
}

void FlvRecorder::onWrite(const Buffer::Ptr &data) {
    lock_guard<recursive_mutex> lck(_file_mtx);
    if(_file){
        fwrite(data->data(),data->size(),1,_file.get());
    }
}

void FlvRecorder::onWrite(const char *data, int len) {
    lock_guard<recursive_mutex> lck(_file_mtx);
    if(_file){
        fwrite(data,len,1,_file.get());
    }
}

void FlvRecorder::onDetach() {
    lock_guard<recursive_mutex> lck(_file_mtx);
    _file.reset();
}

std::shared_ptr<FlvMuxer> FlvRecorder::getSharedPtr() {
    return  shared_from_this();
}

FlvRecorder::FlvRecorder() {
}

FlvRecorder::~FlvRecorder() {
}


    }//namespace Rtmp
}//namespace ZL
