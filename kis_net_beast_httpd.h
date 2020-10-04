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

#ifndef __KIS_NET_BEAST_HTTPD_H__
#define __KIS_NET_BEAST_HTTPD_H__ 

#include "config.h"

#include <atomic>
#include <list>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>


#include "boost/asio.hpp"
#include "boost/beast.hpp"
#include "boost/optional.hpp"

#include "globalregistry.h"
#include "json/json.h"
#include "kis_mutex.h"
#include "messagebus.h"

class kis_net_beast_httpd_connection;
class kis_net_beast_route;
class kis_net_beast_auth;

class kis_net_beast_httpd : public lifetime_global, public deferred_startup,
    public std::enable_shared_from_this<kis_net_beast_httpd> {
public:
    static std::string global_name() { return "BEAST_HTTPD_SERVER"; }
    static std::shared_ptr<kis_net_beast_httpd> create_httpd();

    using http_var_map_t = std::unordered_map<std::string, std::string>;
    // Under this design, all the connection data, uri, variables, streams, etc are stored in the
    // connection record; we don't need to pass anything else
    using http_handler_t = std::function<void (std::shared_ptr<kis_net_beast_httpd_connection>)>;

    virtual void trigger_deferred_startup() override;

    const static std::string LOGON_ROLE;

private:
    kis_net_beast_httpd(boost::asio::ip::tcp::endpoint& endpoint);

public:
    virtual ~kis_net_beast_httpd();

    int start_httpd();
    int stop_httpd();

    bool httpd_running() { return running; }
    unsigned int fetch_port() { return port; }
    bool fetch_using_ssl() { return use_ssl; }

    static std::string decode_uri(boost::beast::string_view in);
    static void decode_variables(const boost::beast::string_view decoded, http_var_map_t& var_map);
    static std::string escape_html(const std::string& html);

    void register_mime_type(const std::string& extension, const std::string& type);
    void remove_mime_type(const std::string& extension);
    std::string resolve_mime_type(const std::string& extension);

    // The majority of routing requires authentication.  Any route that operates outside of 
    // authentication must *explicitly* register as unauthenticated.
    void register_route(const std::string& route, const std::list<std::string>& verbs, 
            const std::string& role, http_handler_t handler);
    void register_route(const std::string& route, const std::list<std::string>& verbs, 
            const std::string& role, const std::list<std::string>& extensions, http_handler_t handler);
    void remove_route(const std::string& route);

    void register_unauth_route(const std::string& route, const std::list<std::string>& verbs, 
            http_handler_t handler);
    void register_unauth_route(const std::string& route, const std::list<std::string>& verbs,
            const std::list<std::string>& extensions, 
            http_handler_t handler);


    // Create an auth entry & return it
    std::string create_auth(const std::string& name, const std::string& role, time_t expiry);
    // Remove an auth entry based on token
    void remove_auth(const std::string& token);
    void load_auth();
    void store_auth();

    std::shared_ptr<kis_net_beast_auth> check_auth_token(const boost::beast::string_view& token);
    bool check_admin_login(const std::string& username, const std::string& password);


    // Map a content directory into the virtual paths
    void register_static_dir(const std::string& prefix, const std::string& path);



    // Find an endpoint
    std::shared_ptr<kis_net_beast_route> find_endpoint(std::shared_ptr<kis_net_beast_httpd_connection> con);


    const bool& allow_cors() { return allow_cors_; }
    const std::string& allowed_cors_referrer() { return allowed_cors_referrer_; }



protected:
    std::atomic<bool> running;
    unsigned int port;

    kis_recursive_timed_mutex mime_mutex;
    std::unordered_map<std::string, std::string> mime_map;

    kis_recursive_timed_mutex route_mutex;
    std::vector<std::shared_ptr<kis_net_beast_route>> route_vec;

    kis_recursive_timed_mutex auth_mutex;
    std::vector<std::shared_ptr<kis_net_beast_auth>> auth_vec;

    kis_recursive_timed_mutex static_mutex;
    class static_content_dir {
    public:
        static_content_dir(const std::string& prefix, const std::string& path) :
            prefix{prefix},
            path{path} { }

        std::string prefix;
        std::string path;
    };
    std::vector<static_content_dir> static_dir_vec;


    boost::asio::ip::tcp::endpoint endpoint;
    boost::asio::ip::tcp::acceptor acceptor;

    void start_accept();
    void handle_connection(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);

    bool use_ssl;
    bool serve_files;

    bool allow_cors_;
    std::string allowed_cors_referrer_;

    // Yes, these are stored in ram.  yes, I'm ok with this.
    std::string admin_username, admin_password;
    bool global_login_config;

    void set_admin_login(const std::string& username, const std::string& password);
};

struct future_streambuf_timeout : public std::exception {
    const char *what() const throw () {
        return "timed out";
    }
};

struct future_streambuf : public boost::asio::streambuf {
public:
    future_streambuf(size_t chunk = 1024) :
        boost::asio::streambuf{},
        chunk{chunk}, 
        blocking{false},
        done{false} { }

    void cancel() {
        done = true;
        sync();
    }

    void complete() {
        done = true;
        sync();
    }

    bool is_complete() const {
        return done;
    }

    size_t wait() {
        if (blocking)
            throw std::runtime_error("future_streambuf already blocking");

        if (done || size())
            return size();

        blocking = true;
        
        data_available_pm = std::promise<bool>();

        auto ft = data_available_pm.get_future();

        ft.wait();

        blocking = false;

        return size();
    }

    template<class Rep, class Period>
    size_t wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        if (blocking)
            throw std::runtime_error("future_stream already blocking");

        if (done || size())
            return size();

        blocking = true;
        
        data_available_pm = std::promise<bool>();

        auto ft = data_available_pm.get_future();

        auto r = ft.wait_for(timeout);
        if (r == std::future_status::timeout)
            throw future_stringbuf_timeout();
        else if (r == std::future_status::deferred)
            throw std::runtime_error("future_stream blocked with no future");

        blocking = false;

        return size();
    }

    virtual std::streamsize xsputn(const char_type *s, std::streamsize n) override {
        auto r = boost::asio::streambuf::xsputn(s, n);

        if (size() >= chunk)
            sync();

        return r;
    }

    virtual int_type overflow(int_type ch) override {
        auto r = boost::asio::streambuf::overflow(ch);

        if (size() >= chunk)
            sync();

        return r;
    }

    virtual int sync() override {
        auto r = boost::asio::streambuf::sync();

        if (blocking) {
            try {
                data_available_pm.set_value(true);
            } catch (const std::future_error& e) {
                ;
            }
        }

        return r;
    }

protected:
    size_t chunk;
    std::atomic<bool> blocking;
    std::atomic<bool> done;
    std::promise<bool> data_available_pm;
};


// Central entity which tracks everything about a connection, parsed variables, generator thread, etc.
class kis_net_beast_httpd_connection : public std::enable_shared_from_this<kis_net_beast_httpd_connection> {
public:
    friend class kis_net_beast_httpd;

    kis_net_beast_httpd_connection(boost::asio::ip::tcp::socket socket,
            std::shared_ptr<kis_net_beast_httpd> httpd);

    using uri_param_t = std::unordered_map<std::string, std::string>;

    void start();

    boost::beast::http::request<boost::beast::http::string_body>& request() { return request_; }
    boost::beast::http::verb& verb() { return verb_; }

    // Stream suitable for std::ostream
    future_streambuf& response_stream() { return response_stream_; }

    // Login validity
    bool login_valid() const { return login_valid_; }
    const std::string& login_role() const { return login_role_; }

    // These may be set by the endpoint handler prior to the first writing of data; once the
    // first block of the response has been sent, it is too late to include these and they will 
    // raise a runtime error exception
    void set_status(unsigned int response);
    void set_status(boost::beast::http::status status);
    void set_mime_type(const std::string& type);
    void set_target_file(const std::string& type);

    const boost::beast::http::verb& verb() const { return verb_; }
    const boost::beast::string_view& uri() const { return uri_; }
    const uri_param_t& uri_params() const { return uri_params_; }
    const kis_net_beast_httpd::http_var_map_t& http_variables() const { return http_variables_; }

protected:
    const std::string AUTH_COOKIE = "KISMET";

    std::shared_ptr<kis_net_beast_httpd> httpd;

    boost::beast::tcp_stream stream;
    boost::beast::flat_buffer buffer;

    boost::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> parser_;
    boost::beast::http::request<boost::beast::http::string_body> request_;

    boost::beast::http::response<boost::beast::http::buffer_body> response;
    future_streambuf response_stream_;

    // Request type
    boost::beast::http::verb verb_;

    // Login data
    bool login_valid_;
    std::string login_role_;

    // All variables
    kis_net_beast_httpd::http_var_map_t http_variables_;
    // Decoded JSON from post json= or from post json document
    Json::Value json;

    boost::beast::string_view auth_token_;
    boost::beast::string_view uri_;
    uri_param_t uri_params_;

    boost::beast::string_view http_post;

    std::atomic<bool> first_response_write;

    void do_read();
    void handle_read(const boost::system::error_code& ec, size_t sz);
    void handle_write(bool close, const boost::system::error_code& ec, size_t sz);
    void do_close();
};

// Routes map a templated URL path to a callback generator which creates the content.
//
// Routes are formatted of the type /path/:key/etc
//
// Routes constructed with no role match any role.
// Routes constructed with a role match "login" *or* that role.
//
// Routes constructed with *no* extensions list match *only* that route
// Routes constructed with an *empty* extensions list {} match *all file types* and resolve at serialization
// Routes constructed with an explicit extensions list {"json", "prettyjson"} resolve ONLY those types
//
// Keys are extracted from the URL and placed in the uri_params dictionary.
// The FILETYPE key is automatically populated with the extracted request file extension (HTML, JSON, etc)
// The GETVARS key is automatically populated with the raw HTTP GET variables string
class kis_net_beast_route {
public:
    kis_net_beast_route(const std::string& route, const std::list<boost::beast::http::verb>& verbs,
            bool login, const std::string& role, kis_net_beast_httpd::http_handler_t handler);
    kis_net_beast_route(const std::string& route, const std::list<boost::beast::http::verb>& verbs,
            bool login, const std::string& role, const std::list<std::string>& extensions, 
            kis_net_beast_httpd::http_handler_t handler);

    // Does a URL match this route?  If so, populate uri params and uri variables
    bool match_url(const std::string& url, kis_net_beast_httpd_connection::uri_param_t& uri_params,
            kis_net_beast_httpd::http_var_map_t& uri_variables);

    // Is the verb compatible?
    bool match_verb(boost::beast::http::verb verb);

    // Is the role compatible?
    bool match_role(bool login, const std::string& role);
    
    // Invoke our registered callback
    void invoke(std::shared_ptr<kis_net_beast_httpd_connection> connection);

    std::string& route() { return route_; }

protected:
    kis_net_beast_httpd::http_handler_t handler;

    std::string route_;

    std::list<boost::beast::http::verb> verbs_;

    bool login_;
    std::string role_;

    const std::string path_id_pattern = ":([^\\/]+)?";
    const std::string path_capture_pattern = "(?:([^\\/]+?))";
    const std::regex path_re = std::regex(path_id_pattern);

    bool match_types;
    std::vector<std::string> match_keys;

    std::regex match_re;
};

struct auth_construction_error : public std::exception {
    const char *what() const throw () {
        return "could not process auth json";
    }
};

// Authentication record, loading from the http auth file.  Authentication tokens may have 
// optional role; a role of '*' has full access to all capabilities.  The meaning of roles is
// defined by the endpoints
class kis_net_beast_auth {
public:
    kis_net_beast_auth(const Json::Value& json);
    kis_net_beast_auth(const std::string& token, const std::string& name, 
            const std::string& role, time_t expires);

    bool check_auth(const boost::beast::string_view& token) const;

    const std::string& token() { return token_; }
    const std::string& name() { return name_; }
    const std::string& role() { return role_; }

    bool is_valid() const { return time_expires_ != 0 && time_expires_ < time(0); }
    void access() { time_accessed_ = time(0); }

    Json::Value as_json();

protected:
    std::string token_;
    std::string name_;
    std::string role_;
    time_t time_created_, time_accessed_, time_expires_;

};

// Basic constant-time string compare for passwords and session keys
struct boost_stringview_constant_time_string_compare_ne {
    bool operator()(const boost::beast::string_view& a, const boost::beast::string_view& b) const {
        bool r = true;

        if (a.length() != b.length())
            r = false;

        for (size_t x = 0; x < a.length() && x < b.length(); x++) {
            if (a[x] != b[x])
                r = false;
        }

        return r == false;
    }

};

#endif /* ifndef KIS_NET_BEAST_HTTPD_H */
