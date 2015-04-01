/**
 * Copyright (c) 2015, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* for daemon */
#include <unistd.h>

/* for mkdir */
#include <sys/stat.h>

#include "h2o.h"
#include "h2o/http1.h"
#include "lmdb.h"
#include "kstr.h"
#include "docopt.c"

#include "pear.h"

#include "assert.h"

server_t server;
server_t *sv = &server;

static void __register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(
                                   h2o_handler_t *,
                                   h2o_req_t *))
{
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path);
    h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
    handler->on_req = on_req;
}

static int __put(h2o_req_t *req, kstr_t* key)
{
    static h2o_generator_t generator = { NULL, NULL };
    MDB_txn *txn;
    int e;

    e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
    {
        perror("can't create transaction");
        abort();
    }

    MDB_val k = { .mv_size = key->len,
                  .mv_data = key->s };
    MDB_val v = { .mv_size = req->entity.len,
                  .mv_data = (void*)req->entity.base };

    e = mdb_put(txn, sv->docs, &k, &v, 0);
    if (0 != e)
    {
        perror("mdm put failed");
        abort();
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
    {
        perror("can't commit transaction");
        abort();
    }

    h2o_iovec_t body;
    body.len = 0;
    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool,
                   &req->res.headers,
                   H2O_TOKEN_CONTENT_TYPE,
                   H2O_STRLIT("text/plain; charset=utf-8"));
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 0;

fail:
    req->res.status = 400;
    req->res.reason = "BAD";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 1;
}

static int __get(h2o_req_t *req, kstr_t* key)
{
    static h2o_generator_t generator = { NULL, NULL };
    h2o_iovec_t body;
    MDB_txn *txn;
    int e;

    e = mdb_txn_begin(sv->db_env, NULL, MDB_RDONLY, &txn);
    if (0 != e)
    {
        perror("can't create transaction");
        abort();
    }

    MDB_val k = { .mv_size = key->len,
                  .mv_data = key->s };
    MDB_val v;

    e = mdb_get(txn, sv->docs, &k, &v);
    switch (e)
    {
    case 0:
        break;
    case MDB_NOTFOUND:
        e = mdb_txn_commit(txn);
        if (0 != e)
        {
            perror("can't commit transaction");
            abort();
        }
        req->res.status = 404;
        req->res.reason = "NOT FOUND";
        body.base = "";
        body.len = 0;
        h2o_add_header(&req->pool,
                       &req->res.headers,
                       H2O_TOKEN_CONTENT_LENGTH,
                       H2O_STRLIT("0"));
        h2o_start_response(req, &generator);
        /* force keep-alive */
        req->http1_is_persistent = 1;
        h2o_send(req, &body, 1, 1);
        return 0;
    default:
        goto fail;
    }

    if (0 != e)
    {
        perror("mdm get failed");
        abort();
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
    {
        perror("can't commit transaction");
        abort();
    }

    body.base = v.mv_data;
    body.len = v.mv_size;
    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool,
                   &req->res.headers,
                   H2O_TOKEN_CONTENT_TYPE,
                   H2O_STRLIT("text/plain; charset=utf-8"));
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 0;
fail:
    body.len = 0;
    req->res.status = 400;
    req->res.reason = "BAD";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return -1;
}

static int __delete(h2o_req_t *req, kstr_t* key)
{
    static h2o_generator_t generator = { NULL, NULL };
    h2o_iovec_t body;
    int e;

    body.len = 0;

    MDB_txn *txn;

    e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
    {
        perror("can't create transaction");
        abort();
    }

    MDB_val k = { .mv_size = key->len,
                  .mv_data = key->s };

    e = mdb_del(txn, sv->docs, &k, NULL);
    switch (e)
    {
    case 0:
        break;
    case MDB_NOTFOUND:
        e = mdb_txn_commit(txn);
        if (0 != e)
        {
            perror("can't commit transaction");
            abort();
        }
        req->res.status = 404;
        req->res.reason = "NOT FOUND";
        body.len = 0;
        h2o_start_response(req, &generator);
        h2o_send(req, &body, 1, 1);
        return 0;
    default:
        goto fail;
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
    {
        perror("can't commit transaction");
        abort();
    }

    req->res.status = 200;
    req->res.reason = "OK";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 0;
fail:
    req->res.status = 400;
    req->res.reason = "BAD";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return -1;
}

static int __pear(h2o_handler_t * self, h2o_req_t * req)
{
    h2o_iovec_t body;
    static h2o_generator_t generator = { NULL, NULL };

    /* get key */
    char* end;
    kstr_t key;
    key.s = req->path.base + 1;
    end = strchr(key.s, '/');
    if (!end)
        goto fail;
    key.len = end - key.s;

    if (h2o_memis(req->method.base, req->method.len, H2O_STRLIT("PUT")))
        return __put(req, &key);
    else if (h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")))
        return __get(req, &key);
    else if (h2o_memis(req->method.base, req->method.len, H2O_STRLIT("DELETE")))
        return __delete(req, &key);

fail:
    body.len = 0;
    req->res.status = 400;
    req->res.reason = "BAD";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 0;
}

static void __db_env_create(MDB_dbi *dbi, MDB_env **env, const char* path)
{
    int e;

    e = mkdir(path, 0777);

    e = mdb_env_create(env);
    if (0 != e)
    {
        perror("can't create lmdb env");
        abort();
    }

    e = mdb_env_set_mapsize(*env, 1048576000);
    if (0 != e)
    {
        perror("can't set map size");
        abort();
    }

    e = mdb_env_set_maxdbs(*env, 1024);
    if (0 != e)
    {
        perror(mdb_strerror(e));
        abort();
    }

    e = mdb_env_open(*env, path,  MDB_WRITEMAP, 0664);
    if (0 != e)
    {
        perror(mdb_strerror(e));
        abort();
    }
}

static void __db_create(MDB_dbi *dbi, MDB_env *env, const char* db_name)
{
    int e;
    MDB_txn *txn;

    e = mdb_txn_begin(env, NULL, 0, &txn);
    if (0 != e)
    {
        perror("can't create transaction");
        abort();
    }

    e = mdb_dbi_open(txn, db_name, MDB_CREATE, dbi);
    if (0 != e)
    {
        perror("can't create lmdb db");
        abort();
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
    {
        perror("can't create transaction");
        abort();
    }
}

/**
 * workers connect to listen thread via pipe
 */
void __spawn_workers()
{
    int i;
    for (i = 0; i < WORKER_THREADS; i++)
    {
        pear_thread_t* thread = &sv->threads[i + 1];
        uv_sem_init(&thread->sem, 0);
        uv_thread_create(&thread->thread, pear_worker_loop, i + 1);
    }
}

int main(int argc, char **argv)
{
    DocoptArgs args = docopt(argc, argv, 1, "0.1");

    __db_env_create(&sv->docs, &sv->db_env,
                    args.db_path ? args.db_path : "store");
    __db_create(&sv->docs, sv->db_env, "docs");

    if (args.daemonize)
    {
        int ret = daemon(1, 0);
        if (-1 == ret)
            abort();
    }
    else
        signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&sv->cfg);
    h2o_hostconf_t *hostconf = h2o_config_register_host(&sv->cfg, "default");
    __register_handler(hostconf, "/", __pear);

    uv_barrier_init(&sv->listeners_created_barrier, THREADS);

    sv->threads = alloca(sizeof(sv->threads[0]) * THREADS);

    __spawn_workers();

    pear_listen_loop((void*)0);

fail:
    return 1;
}