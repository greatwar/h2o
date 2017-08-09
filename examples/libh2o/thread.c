/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "h2o.h"
#include "h2o/http1.h"
#include "h2o/http2.h"

#include "threadpool/thpool.h"


static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path,0);
    h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
    handler->on_req = on_req;
    return pathconf;
}

static int echo_test(h2o_handler_t *self, h2o_req_t *req)
{
	static h2o_generator_t generator = {NULL, NULL};
	h2o_iovec_t body = h2o_strdup(&req->pool, "hello world\n", SIZE_MAX);
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE,NULL, H2O_STRLIT("text/plain"));
	h2o_start_response(req, &generator);
	h2o_send(req, &body, 1, 1);
	return 0;
}

static  threadpool thpool;
typedef struct st_demo_thread_req_private_s demo_thread_req_private_t;
struct st_demo_thread_req_private_s{
	h2o_multithread_receiver_t *receiver;
		h2o_linklist_t pending;
		h2o_multithread_message_t message;
		h2o_req_t * req;
};

static pthread_mutex_t thread_mutex;
static h2o_linklist_t thread_pending;
static h2o_multithread_receiver_t thread_receiver;

static void demo_thread_doit(h2o_req_t *req){
		static h2o_generator_t generator = {NULL, NULL};
		h2o_iovec_t body = h2o_strdup(&req->pool, "hello world\n", SIZE_MAX);
		req->res.status = 200;
		req->res.reason = "OK";
		h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
		h2o_start_response(req, &generator);
		h2o_send(req, &body, 1, 1);
}
static void demo_thread_receiver(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages) {
	while (!h2o_linklist_is_empty(messages)) {
		demo_thread_req_private_t *req = H2O_STRUCT_FROM_MEMBER(demo_thread_req_private_t,
					message.link, messages->next);
			h2o_linklist_unlink(&req->message.link);
			demo_thread_doit(req->req);
		}
}

static void *echo_in_thread(void *data)
{
	h2o_req_t *req = data;
	
	demo_thread_req_private_t *preq = h2o_mem_alloc_pool(&req->pool,sizeof(demo_thread_req_private_t));

	preq->req = req;
	
	preq->receiver = &thread_receiver;
	preq->pending = (h2o_linklist_t ) { };
	preq->message = (h2o_multithread_message_t ) {{NULL}};

	pthread_mutex_lock(&thread_mutex);
	h2o_linklist_insert(&thread_pending, &preq->pending);
	pthread_mutex_unlock(&thread_mutex);

	h2o_multithread_send_message(preq->receiver, &preq->message);
		
	return NULL;
}

static int threadecho_test(h2o_handler_t *self, h2o_req_t *req) 
{
	thpool_add_work(thpool,echo_in_thread,req);
	return 0;
}


///
///---------------------------- only response in thread
typedef struct http_res_private_s http_res_private_t;
struct http_res_private_s {
    // h2o_multithread_receiver_t *receiver;
    // h2o_linklist_t pending;
    h2o_multithread_message_t message;

    h2o_req_t *req;
    h2o_iovec_t *body;
};
static inline void http_response_text(h2o_req_t *req, char *text, size_t len, char *content_type, int code,
                                      char *reason) ;
static void *echo2_in_thread(void *data)
{
    h2o_req_t *req = data;
    
    //sleep(1);
    
    http_response_text(req,"hello world",11,"text/plain",200,NULL);
        
    return NULL;
}
static int threadecho2_test(h2o_handler_t *self, h2o_req_t *req) 
{
    thpool_add_work(thpool,echo2_in_thread,req);
    return 0;
}
pthread_t service_share_thread_id;
h2o_multithread_receiver_t service_share_res_receiver={};

static void http_res_doit(h2o_req_t *req, h2o_iovec_t *body) {
//    T("in thread %lx", pthread_self());
    h2o_generator_t generator = {NULL, NULL};
    h2o_start_response(req, &generator);
    h2o_send(req, body, 1, 1);
}

static void http_res_receive(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages) {
    while (!h2o_linklist_is_empty(messages)) {
        http_res_private_t *req = H2O_STRUCT_FROM_MEMBER(http_res_private_t, message.link, messages->next);
        h2o_linklist_unlink(&req->message.link);
        http_res_doit(req->req, req->body);
    }
}

static long int tms_now(void) {
    struct timespec ts = {0, 0};
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    return ((long int)ts.tv_sec) * 1000 + (((long int)ts.tv_nsec) / 1000000);
}

static inline void http_response_text(h2o_req_t *req, char *text, size_t len, char *content_type, int code,
                                      char *reason) {
    if (code < 1)
        code = 200;
    if (reason == NULL)
        reason = "OK";
    if (content_type == NULL)
        content_type = "text/plain; charset=utf-8";

    h2o_iovec_t *body = h2o_mem_alloc_pool(&req->pool, sizeof(h2o_iovec_t));
    body->base = h2o_mem_alloc_pool(&req->pool, len + 1);
    memcpy(body->base, text, len);
    body->base[len] = '\0';
    body->len = len;

    req->res.status = code;
    req->res.reason = reason;
    req->res.content_length = len;
    h2o_add_header_by_str(&req->pool, &req->res.headers, "Content-Type", 12, 0, NULL, content_type,
                          strnlen(content_type, 128));
    if (pthread_equal(service_share_thread_id, pthread_self())) {
        h2o_generator_t generator = {NULL, NULL};
        h2o_start_response(req, &generator);
        h2o_send(req, body, 1, 1);
    } else {
        printf("-");
        http_res_private_t *preq = h2o_mem_alloc_pool(&req->pool, sizeof(http_res_private_t));
        preq->req = req;
        preq->body = body;

        // preq->receiver = &aux->res_receiver;
        // preq->pending = (h2o_linklist_t ) { };
        preq->message = (h2o_multithread_message_t){{NULL}};

        // pthread_mutex_lock(&aux->res_mutex);
        // h2o_linklist_insert(&aux->res_pending, &preq->pending);
        // pthread_mutex_unlock(&aux->res_mutex);

        //  h2o_multithread_send_message(preq->receiver, &preq->message);
        h2o_multithread_send_message(&service_share_res_receiver, &preq->message);
    }
}
///----------------------------
static h2o_globalconf_t config;
static h2o_context_t ctx;
static h2o_accept_ctx_t accept_ctx;

#if H2O_USE_LIBUV

static void on_accept(uv_stream_t *listener, int status)
{
    uv_tcp_t *conn;
    h2o_socket_t *sock;

    if (status != 0)
        return;

    conn = h2o_mem_alloc(sizeof(*conn));
    uv_tcp_init(listener->loop, conn);

    if (uv_accept(listener, (uv_stream_t *)conn) != 0) {
        uv_close((uv_handle_t *)conn, (uv_close_cb)free);
        return;
    }

    sock = h2o_uv_socket_create((uv_stream_t *)conn, (uv_close_cb)free);
    h2o_accept(&accept_ctx, sock);
}

static int create_listener(void)
{
    static uv_tcp_t listener;
    struct sockaddr_in addr;
    int r;

    uv_tcp_init(ctx.loop, &listener);
    uv_ip4_addr("127.0.0.1", 7890, &addr);
    if ((r = uv_tcp_bind(&listener, (struct sockaddr *)&addr, 0)) != 0) {
        fprintf(stderr, "uv_tcp_bind:%s\n", uv_strerror(r));
        goto Error;
    }
    if ((r = uv_listen((uv_stream_t *)&listener, 128, on_accept)) != 0) {
        fprintf(stderr, "uv_listen:%s\n", uv_strerror(r));
        goto Error;
    }

    return 0;
Error:
    uv_close((uv_handle_t *)&listener, NULL);
    return r;
}

#else

static void on_accept(h2o_socket_t *listener, const char *err)
{
    h2o_socket_t *sock;

    if (err != NULL) {
            return;
        }
    for(int i = 0;i<1024;i++) {
    if ((sock = h2o_evloop_socket_accept(listener)) == NULL)
        break;
    printf("a");
    h2o_accept(&accept_ctx, sock);
    }
}

static int create_listener(void)
{
    struct sockaddr_in addr;
    int fd, reuseaddr_flag = 1;
    h2o_socket_t *sock;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    //addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = htons(7890);

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0) {
        return -1;
    }

    sock = h2o_evloop_socket_create(ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    h2o_socket_read_start(sock, on_accept);

    return 0;
}

#endif

int main(int argc, char **argv)
{
    h2o_hostconf_t *hostconf;

    signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&config);
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    register_handler(hostconf, "/echo", echo_test);
    register_handler(hostconf, "/threadecho", threadecho_test);
    register_handler(hostconf, "/threadecho2", threadecho2_test);

#if H2O_USE_LIBUV
    uv_loop_t loop;
    uv_loop_init(&loop);
    h2o_context_init(&ctx, &loop, &config);
#else
    h2o_context_init(&ctx, h2o_evloop_create(), &config);
#endif

    /*thread usage demo*/
    int thread_num = 40;
    char *poolsz = getenv("poolsz");
    if(poolsz){
    	thread_num=atoi(poolsz);
    }
    fprintf(stderr,"thread pool size %d\n",thread_num);
    thpool = thpool_init(thread_num);
    thread_pending = (h2o_linklist_t ) { &thread_pending, &thread_pending };
	thread_receiver = (h2o_multithread_receiver_t ) { };
    pthread_mutex_init(&thread_mutex, NULL);
    h2o_multithread_register_receiver(ctx.queue, &thread_receiver, demo_thread_receiver);

    service_share_thread_id = pthread_self();
    h2o_multithread_register_receiver(ctx.queue, &service_share_res_receiver, http_res_receive);
    
    /* disabled by default: uncomment the line below to enable access logging */
    /* h2o_access_log_register(&config.default_host, "/dev/stdout", NULL); */

    accept_ctx.ctx = &ctx;
    accept_ctx.hosts = config.hosts;

    if (create_listener() != 0) {
        fprintf(stderr, "failed to listen to 127.0.0.1:7890:%s\n", strerror(errno));
        goto Error;
    }

#if H2O_USE_LIBUV
    uv_run(ctx.loop, UV_RUN_DEFAULT);
#else
    while (h2o_evloop_run(ctx.loop,INT32_MAX) == 0)
        ;
#endif

Error:
    return 1;
}
