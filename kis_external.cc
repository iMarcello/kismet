/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <memory>

#include "kis_external.h"
#include "kis_external_packet.h"

#include "endian_magic.h"

#include "protobuf_cpp/kismet.pb.h"
#include "protobuf_cpp/http.pb.h"

KisExternalInterface::KisExternalInterface(GlobalRegistry *in_globalreg) :
    Kis_Net_Httpd_Chain_Stream_Handler(in_globalreg) {
    globalreg = in_globalreg;

    timetracker = 
        Globalreg::FetchMandatoryGlobalAs<Timetracker>(globalreg, "TIMETRACKER");

    seqno = 0;
    http_session_id = 0;

    last_pong = 0;

    ping_timer_id = -1;

}

KisExternalInterface::~KisExternalInterface() {
    local_eol_locker el(&ext_mutex);

    timetracker->RemoveTimer(ping_timer_id);

    // Kill any active sessions
    for (auto s : http_proxy_session_map) {
        // Fail them
        s.second->connection->httpcode = 501;
        // Unlock them and let the cleanup in the thread handle it and close down 
        // the http server session
        s.second->locker->unlock();
    }

    // If we have a ringbuf handler, remove ourselves as the interface, trigger an error
    // to shut it down, and delete our shared reference to it
    if (ringbuf_handler != NULL) {
        ringbuf_handler->RemoveReadBufferInterface();
        ringbuf_handler->ProtocolError();
        ringbuf_handler.reset();
    }

    // Remove the IPC remote reference
    ipc_remote.reset();
}

void KisExternalInterface::connect_buffer(std::shared_ptr<BufferHandlerGeneric> in_ringbuf) {
    local_locker lock(&ext_mutex);

    if (ringbuf_handler != NULL && ringbuf_handler != in_ringbuf) {
        ringbuf_handler.reset();
    }

    ringbuf_handler = in_ringbuf;
    ringbuf_handler->SetReadBufferInterface(this);
}

void KisExternalInterface::trigger_error(std::string in_error) {
    local_locker lock(&ext_mutex);

    timetracker->RemoveTimer(ping_timer_id);

    // Kill any active sessions
    for (auto s : http_proxy_session_map) {
        // Fail them
        s.second->connection->httpcode = 501;
        // Unlock them and let the cleanup in the thread handle it and close down 
        // the http server session
        s.second->locker->unlock();
    }

    // If we have a ringbuf handler, remove ourselves as the interface, trigger an error
    // to shut it down, and delete our shared reference to it
    if (ringbuf_handler != NULL) {
        ringbuf_handler->RemoveReadBufferInterface();
        ringbuf_handler->ProtocolError();
        ringbuf_handler.reset();
    }

    // Remove the IPC remote reference
    ipc_remote.reset();

    BufferError(in_error);
}

void KisExternalInterface::BufferAvailable(size_t in_amt __attribute__((unused))) {
    local_locker lock(&ext_mutex);

    kismet_external_frame_t *frame;
    uint8_t *buf = NULL;
    uint32_t frame_sz, data_sz;
    uint32_t data_checksum;

    // Consume everything in the buffer that we can
    while (1) {
        if (ringbuf_handler == NULL)
            return;

        // See if we have enough to get a frame header
        size_t buffamt = ringbuf_handler->GetReadBufferUsed();

        if (buffamt < sizeof(kismet_external_frame_t))
            return;

        // Peek at the header
        buffamt = ringbuf_handler->PeekReadBufferData((void **) &buf, buffamt);

        // Make sure we got the right amount
        if (buffamt < sizeof(kismet_external_frame_t)) {
            ringbuf_handler->PeekFreeReadBufferData(buf);
            return;
        }

        // Turn it into a frame header
        frame = (kismet_external_frame_t *) buf;

        // Check the frame signature
        if (kis_ntoh32(frame->signature) != KIS_EXTERNAL_PROTO_SIG) {
            ringbuf_handler->PeekFreeReadBufferData(buf);

            _MSG("Kismet external interface got command frame with invalid signature", MSGFLAG_ERROR);
            trigger_error("Invalid signature on command frame");

            return;
        }

        // Check the length
        data_sz = kis_ntoh32(frame->data_sz);
        frame_sz = data_sz + sizeof(kismet_external_frame);

        // If we'll never be able to read it, blow up
        if (frame_sz >= ringbuf_handler->GetReadBufferAvailable()) {
            ringbuf_handler->PeekFreeReadBufferData(buf);

            _MSG("Kismet external interface got command frame too large to ever be read", 
                    MSGFLAG_ERROR);
            trigger_error("Command frame too large for buffer");

            return;
        }

        // If we don't have the whole buffer available, bail on this read
        if (frame_sz < buffamt) {
            ringbuf_handler->PeekFreeReadBufferData(buf);
            return;
        }

        // We have a complete payload, checksum 
        data_checksum = Adler32Checksum((const char *) frame->data, data_sz);

        if (data_checksum != kis_ntoh32(frame->data_checksum)) {
            ringbuf_handler->PeekFreeReadBufferData(buf);

            _MSG("Kismet external interface got command frame with invalid checksum",
                    MSGFLAG_ERROR);
            trigger_error("command frame has invalid checksum");

            return;
        }

        // Process the data payload as a protobuf frame
        std::shared_ptr<KismetExternal::Command> cmd(new KismetExternal::Command());

        if (!cmd->ParseFromArray(frame->data, data_sz)) {
            ringbuf_handler->PeekFreeReadBufferData(buf);

            _MSG("Kismet external interface could not interpret the payload of the "
                    "command frame", MSGFLAG_ERROR);
            trigger_error("unparseable command frame");

            return;
        }

        fprintf(stderr, "debug - KISEXTERNALAPI got command '%s' seq %u sz %lu\n",
                cmd->command().c_str(), cmd->seqno(), cmd->content().length());

        // Consume the buffer now that we're done; we only consume the 
        // frame size because we could have peeked a much larger buffer
        ringbuf_handler->PeekFreeReadBufferData(buf);
        ringbuf_handler->ConsumeReadBufferData(frame_sz);

        // Dispatch the received command
        dispatch_rx_packet(cmd);
    }
}

void KisExternalInterface::BufferError(std::string in_error) {
    close_external();
}

bool KisExternalInterface::launch_ipc() {
    local_locker lock(&ext_mutex);

    std::stringstream ss;

    if (external_binary == "") {
        _MSG("Kismet external interface did not have an IPC binary to launch", MSGFLAG_ERROR);

        return false;
    }

    return true;
}

void KisExternalInterface::close_external() {
    local_locker lock(&ext_mutex);

    timetracker->RemoveTimer(ping_timer_id);

    if (ringbuf_handler != NULL) {
        ringbuf_handler->RemoveReadBufferInterface();
        ringbuf_handler->ProtocolError();
        ringbuf_handler.reset();
    }

    if (ipc_remote != NULL) 
        ipc_remote->soft_kill();

    // Remove the IPC remote reference
    ipc_remote.reset();
}

bool KisExternalInterface::send_packet(std::shared_ptr<KismetExternal::Command> c) {
    local_locker lock(&ext_mutex);

    if (ringbuf_handler == NULL)
        return false;

    uint32_t data_csum;

    // Get the serialized size of our message
    size_t content_sz = c->ByteSize();

    // Calc frame size
    ssize_t frame_sz = sizeof(kismet_external_frame_t) + content_sz;

    // Our actual frame
    kismet_external_frame_t *frame;

    // Reserve the frame in the buffer
    if (ringbuf_handler->ReserveWriteBufferData((void **) &frame, frame_sz) < frame_sz) {
        ringbuf_handler->CommitWriteBufferData(NULL, 0);
        _MSG("Kismet external interface couldn't find space in the output buffer for "
                "the next command, something may have stalled.", MSGFLAG_ERROR);
        trigger_error("write buffer full");
        return false;
    }

    // Fill in the headers
    frame->signature = kis_hton32(KIS_EXTERNAL_PROTO_SIG);
    frame->data_sz = kis_hton32(content_sz);

    // Serialize into our array
    c->SerializeToArray(frame->data, content_sz);

    // Calculate the checksum and set it in the frame
    data_csum = Adler32Checksum((const char *) frame->data, content_sz); 
    frame->data_checksum = kis_hton32(data_csum);

    // Commit our write buffer
    ringbuf_handler->CommitReadBufferData((void *) frame, frame_sz);

    return true;
}

void KisExternalInterface::dispatch_rx_packet(std::shared_ptr<KismetExternal::Command> c) {
    // Simple dispatcher; this should be called by child implementations who
    // add their own commands
    if (c->command() == "MESSAGE") {
        handle_packet_message(c->seqno(), c->content());
    } else if (c->command() == "PING") {
        handle_packet_ping(c->seqno(), c->content());
    } else if (c->command() == "PONG") {
        handle_packet_pong(c->seqno(), c->content());
    } else if (c->command() == "SHUTDOWN") {
        handle_packet_shutdown(c->seqno(), c->content());
    }
}

void KisExternalInterface::handle_packet_http_register(uint32_t in_seqno, std::string in_content) {
    local_locker lock(&ext_mutex);

    KismetExternalHttp::HttpRegisterUri uri;

    if (!uri.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparseable HTTPREGISTERURI", MSGFLAG_ERROR);
        trigger_error("Invalid HTTPREGISTERURI");
        return;
    }

    struct KisExternalHttpUri *exturi = new KisExternalHttpUri();
    
    exturi->uri = uri.uri();
    exturi->method = uri.method();
    exturi->auth_req = uri.auth_required();

    // Add it to the map of valid URIs
    http_proxy_uri_map[exturi->method].push_back(exturi);
}

void KisExternalInterface::handle_packet_http_response(uint32_t in_seqno, std::string in_content) {
    local_locker lock(&ext_mutex);

    KismetExternalHttp::HttpResponse resp;

    if (!resp.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparseable HTTPRESPONSE", MSGFLAG_ERROR);
        trigger_error("Invalid  HTTPRESPONSE");
        return;
    }

    auto si = http_proxy_session_map.find(resp.req_id());

    if (si == http_proxy_session_map.end()) {
        _MSG("Kismet external interface got a HTTPRESPONSE for an unknown session", MSGFLAG_ERROR);
        trigger_error("Invalid HTTPRESPONSE session");
        return;
    }

    auto session = si->second;

    Kis_Net_Httpd_Buffer_Stream_Aux *saux = 
        (Kis_Net_Httpd_Buffer_Stream_Aux *) session->connection->custom_extension;

    // First off, process any headers we're trying to add, they need to come 
    // before data
    for (int hi = 0; hi < resp.header_content_size() && resp.header_content_size() > 0; hi++) {
        KismetExternalHttp::SubHttpHeader hh = resp.header_content(hi);

        MHD_add_response_header(session->connection->response, hh.header().c_str(), 
                hh.content().c_str());
    }

    // Set any connection state
    if (resp.has_resultcode()) {
        session->connection->httpcode = resp.has_resultcode();
    }

    // Copy any response data
    if (resp.has_content() && resp.content().size() > 0) {
        if (!saux->ringbuf_handler->PutWriteBufferData(resp.content())) {
            _MSG("Kismet external interface could not put response data into the HTTP "
                    "buffer for a HTTPRESPONSE session", MSGFLAG_ERROR);
            // We have to kill this session before we shut down everything else
            session->connection->httpcode = 501;
            session->locker->unlock();
            trigger_error("Unable to write to HTTP buffer in HTTPRESPONSE");
            return;
        }
    }

    // Are we finishing the connection?
    if (resp.has_close_response() && resp.close_response()) {
        // Unlock this session
        session->locker->unlock();
    }
}

void KisExternalInterface::handle_packet_message(uint32_t in_seqno, std::string in_content) {
    KismetExternal::MsgbusMessage m;

    if (!m.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparseable MESSAGE", MSGFLAG_ERROR);
        trigger_error("Invalid MESSAGE");
        return;
    }

    _MSG(m.msgtext(), m.msgtype());
}

void KisExternalInterface::handle_packet_ping(uint32_t in_seqno, std::string in_content) {
   send_pong(in_seqno);
}

void KisExternalInterface::handle_packet_pong(uint32_t in_seqno, std::string in_content) {
    local_locker lock(&ext_mutex);

    KismetExternal::Pong p;
    if (!p.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparseable PONG packet", MSGFLAG_ERROR);
        trigger_error("Invalid PONG");
        return;
    }

    last_pong = time(0);
}

void KisExternalInterface::handle_packet_shutdown(uint32_t in_seqno, std::string in_content) {
    local_locker lock(&ext_mutex);

    KismetExternal::Shutdown s;
    if (!s.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparseable SHUTDOWN", MSGFLAG_ERROR);
        trigger_error("invalid SHUTDOWN");
        return;
    }

    _MSG(std::string("Kismet external interface shutting down: ") + s.reason(), MSGFLAG_INFO); 
    trigger_error(std::string("Remote connection requesting shutdown: ") + s.reason());
}

void KisExternalInterface::send_http_request(uint32_t in_http_sequence, std::string in_uri,
        std::string in_method, std::map<std::string, std::string> in_postdata) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_seqno(seqno++);
    c->set_command("HTTPREQUEST");

    KismetExternalHttp::HttpRequest r;
    r.set_req_id(in_http_sequence);
    r.set_uri(in_uri);
    r.set_method(in_method);

    for (auto pi : in_postdata) {
        KismetExternalHttp::SubHttpPostData *pd = r.add_post_data();
        pd->set_field(pi.first);
        pd->set_field(pi.second);
    }

    c->set_content(r.SerializeAsString());

    send_packet(c);
}

void KisExternalInterface::send_ping() {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_seqno(seqno++);
    c->set_command("PING");

    KismetExternal::Ping p;
    c->set_content(p.SerializeAsString());

    send_packet(c);
}

void KisExternalInterface::send_pong(uint32_t ping_seqno) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_seqno(seqno++);
    c->set_command("PONG");

    KismetExternal::Pong p;
    p.set_ping_seqno(ping_seqno);

    c->set_content(p.SerializeAsString());

    send_packet(c);
}

void KisExternalInterface::send_shutdown(std::string reason) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_seqno(seqno++);
    c->set_command("SHUTDOWN");

    KismetExternal::Shutdown s;
    s.set_reason(reason);

    c->set_content(s.SerializeAsString());

    send_packet(c);
}

bool KisExternalInterface::Httpd_VerifyPath(const char *path, const char *method) {
    local_locker lock(&ext_mutex);

    // Find all the registered endpoints for this method
    auto m = http_proxy_uri_map.find(std::string(method));

    if (m == http_proxy_uri_map.end())
        return false;

    // If an endpoint matches, we're good
    for (auto e : m->second) {
        if (e->uri == std::string(path)) {
            return true;
        }
    }

    return false;
}

// When this function gets called, we're inside a thread for the HTTP server;
// additionally, the HTTP server has it's own thread servicing the chainbuffer
// on the backend of this connection;
//
// Because we are, ourselves, async waiting for the responses from the proxied
// tool, we need to set a lock and sit on it until the proxy has completed.
// We don't need to spawn our own thread - we're already our own thread independent
// of the IO processing system.
int KisExternalInterface::Httpd_CreateStreamResponse(Kis_Net_Httpd *httpd,
        Kis_Net_Httpd_Connection *connection,
        const char *url, const char *method, const char *upload_data,
        size_t *upload_data_size) {

    // Use a demand locker instead of pure scope locker because we need to let it go
    // before we go into blocking wait
    local_demand_locker dlock(&ext_mutex);
    dlock.lock();

    auto m = http_proxy_uri_map.find(std::string(method));

    if (m == http_proxy_uri_map.end()) {
        connection->httpcode = 501;
        return MHD_YES;
    }

    for (auto e : m->second) {
        if (e->uri == std::string(url)) {
            // Make sure we're logged in if we need to be
            if (e->auth_req && !httpd->HasValidSession(connection)) {
                connection->httpcode = 503;
                return MHD_YES;
            }

            // Make a session
            std::shared_ptr<KisExternalHttpSession> s(new KisExternalHttpSession());
            s->connection = connection;
            // Lock the waitlock
            s->locker.reset(new conditional_locker<int>());
            s->locker->lock();

            // Log the session number
            uint32_t sess_id = http_session_id++;
            http_proxy_session_map[sess_id] = s;

            // Send the proxy response
            send_http_request(sess_id, connection->url, std::string(method),
                    std::map<std::string, std::string>());

            // Unlock the demand locker
            dlock.unlock();

            // Block until the external tool sends a connection end; all of the writing
            // to the stream will be handled inside the handle_http_response handler
            // and it will unlock us when we've gotten to the end of the stream.
            s->locker->block_until();

            // Re-acquire the lock
            dlock.lock();

            // Remove the session from our map
            auto mi = http_proxy_session_map.find(sess_id);
            if (mi != http_proxy_session_map.end())
                http_proxy_session_map.erase(mi);

            // The session code should have been set already here so we don't have anything
            // else to do except tell the webserver we're done and let our session
            // de-scope as we exit...
            return MHD_YES;
        }
    }

    connection->httpcode = 501;
    return MHD_YES;
}

int KisExternalInterface::Httpd_PostComplete(Kis_Net_Httpd_Connection *connection) {
    auto m = http_proxy_uri_map.find(std::string("POST"));

    if (m == http_proxy_uri_map.end()) {
        connection->httpcode = 501;
        return MHD_YES;
    }

    for (auto e : m->second) {
        if (e->uri == std::string(connection->url)) {
            // Make sure we're logged in if we need to be
            if (e->auth_req && !httpd->HasValidSession(connection)) {
                connection->httpcode = 503;
                return MHD_YES;
            }

            // TODO process POST

        }
    }

    connection->httpcode = 501;
    return MHD_YES;

    return 0;
}
