/***

Copyright 2013-2016 Dave Cridland
Copyright 2014-2016 Surevine Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

***/

#include "netsession.h"
#include "xmlstream.h"
#include "router.h"
#include "log.h"

#include "rapidxml_print.hpp"

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <cstring>

using namespace Metre;

NetSession::NetSession(long long unsigned serial, struct bufferevent *bev, SESSION_TYPE type)
        : m_serial(serial), m_bev(nullptr), m_xml_stream(new XMLStream(this, INBOUND, type)) {
    bufferevent(bev);
}

NetSession::NetSession(long long unsigned serial, struct bufferevent *bev, std::string const &stream_from,
                       std::string const &stream_to)
        : m_serial(serial), m_bev(nullptr), m_xml_stream(new XMLStream(this, OUTBOUND, S2S, stream_from, stream_to)) {
    bufferevent(bev);
}

void NetSession::bufferevent(struct bufferevent *bev) {
    if (!bev) throw std::runtime_error("NULL BEV");
    bufferevent_setcb(bev, NetSession::read_cb, NULL, NetSession::event_cb, this);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
    m_bev = bev;
}

NetSession::~NetSession() {
    if (m_bev) bufferevent_free(m_bev);
}

bool NetSession::drain() {
    // While there is data, see how much we can consume with the XMLStream.
    struct evbuffer *buf = bufferevent_get_input(m_bev); // TODO
    size_t len;
    while ((len = evbuffer_get_contiguous_space(buf)) > 0) {
        if (!m_xml_stream->closed()) {
            size_t used = m_xml_stream->process(evbuffer_pullup(buf, len), len);
            if (used == 0) {
                break;
            }
            //evbuffer_drain(buf, used);
        } else {
            break;
        }
    }
    // If we can't consume it all, try pullup(), then return.
    if ((len = evbuffer_get_length(buf)) > 0) {
        if (!m_xml_stream->closed()) {
            m_xml_stream->process(evbuffer_pullup(buf, -1), len);
        } else {
            if (len > 0) METRE_LOG(Metre::Log::WARNING, "Stuff left after close: {" << len << "}");
            return true;
        }
    }
    return m_xml_stream->closed() && (evbuffer_get_length(buf) == 0);
}

void NetSession::used(size_t n) {
    struct evbuffer *buf = bufferevent_get_input(m_bev);
    evbuffer_drain(buf, n);
}

void NetSession::send(rapidxml::xml_document<> &d) {
    std::string tmp;
    rapidxml::print(std::back_inserter(tmp), d, rapidxml::print_no_indenting);
    struct evbuffer *buf = bufferevent_get_output(m_bev);
    METRE_LOG(Metre::Log::DEBUG, this << " - Send: " << m_xml_stream << ": " << tmp);
    evbuffer_add(buf, tmp.data(),
                 tmp.length()); // Crappy and inefficient; we want to generate a char *, write directly to it, and dump it into an iovec.
}

void NetSession::send(std::string const &s) {
    struct evbuffer *buf = bufferevent_get_output(m_bev);
    METRE_LOG(Metre::Log::DEBUG, this << " - Send: " << m_xml_stream << ": " << s);
    evbuffer_add(buf, s.data(), s.length());
}

void NetSession::send(const char *p) {
    send(std::string(p));
    //struct evbuffer * buf = bufferevent_get_output(m_bev);
    //evbuffer_add(buf, p, std::strlen(p));
}

void NetSession::read_cb(struct bufferevent *, void *arg) {
    NetSession &ns = *reinterpret_cast<NetSession *>(arg);
    if (ns.drain()) ns.onClosed.emit(ns);
}

void NetSession::bev_closed() {
    onClosed.emit(*this);
}

void NetSession::bev_connected() {
    onConnected.emit(*this);
    m_xml_stream->restart();
}

void NetSession::event_cb(struct bufferevent *, short events, void *arg) {
    METRE_LOG(Metre::Log::DEBUG, "Events have happened.");
    NetSession &ns = *reinterpret_cast<NetSession *>(arg);
    if (events & BEV_EVENT_ERROR) {
        ns.bev_closed();
    } else if (events & BEV_EVENT_EOF) {
        ns.bev_closed();
    } else if (events & BEV_EVENT_CONNECTED) {
        METRE_LOG(Metre::Log::DEBUG, "Connected.");
        ns.bev_connected();
    }
}

void NetSession::close() {
    bufferevent_flush(m_bev, EV_WRITE, BEV_FINISHED);
    onClosed.emit(*this);
}
