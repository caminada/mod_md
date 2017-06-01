/* Copyright 2017 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <apr_lib.h>
#include <apr_file_info.h>
#include <apr_file_io.h>
#include <apr_fnmatch.h>
#include <apr_hash.h>
#include <apr_strings.h>

#include "md.h"
#include "md_crypt.h"
#include "md_log.h"
#include "md_json.h"
#include "md_store.h"
#include "md_util.h"

/**************************************************************************************************/
/* generic callback handling */

void md_store_destroy(md_store_t *store)
{
    if (store->destroy) store->destroy(store);
}

apr_status_t md_store_load_mds(apr_array_header_t *mds, md_store_t *store, apr_pool_t *p)
{
    return store->load_mds(mds, store, p);
}

apr_status_t md_store_load_md(md_t **pmd, md_store_t *store, const char *name, apr_pool_t *p)
{
    return store->load_value((void**)pmd, store, name, MD_STORE_V_MD, p);
}

apr_status_t md_store_save_md(md_store_t *store, md_t *md, int create)
{
    return store->save_value(store, md->name, MD_STORE_V_MD, md, create);
}

apr_status_t md_store_remove_md(md_store_t *store, const char *name, int force)
{
    return store->remove_value(store, name, MD_STORE_V_MD, force);
}

apr_status_t md_store_load_cert(struct md_cert_t **pcert, md_store_t *store, 
                                const char *name, apr_pool_t *p)
{
    return store->load_value((void**)pcert, store, name, MD_STORE_V_CERT, p);
}

apr_status_t md_store_save_cert(md_store_t *store, const char *name, struct md_cert_t *cert)
{
    return store->save_value(store, name, MD_STORE_V_CERT, cert, 0);
}

apr_status_t md_store_load_pkey(struct md_pkey_t **ppkey, md_store_t *store, 
                                const char *name, apr_pool_t *p)
{
    return store->load_value((void**)ppkey, store, name, MD_STORE_V_PKEY, p);
}

apr_status_t md_store_save_pkey(md_store_t *store, const char *name, struct md_pkey_t *pkey)
{
    return store->save_value(store, name, MD_STORE_V_PKEY, pkey, 0);
}

apr_status_t md_store_load_chain(struct apr_array_header_t **pchain, md_store_t *store, 
                                const char *name, apr_pool_t *p)
{
    return store->load_value((void**)pchain, store, name, MD_STORE_V_CHAIN, p);
}

apr_status_t md_store_save_chain(md_store_t *store, const char *name,
                                 struct apr_array_header_t *chain)
{
    return store->save_value(store, name, MD_STORE_V_CHAIN, chain, 0);
}

/**************************************************************************************************/
/* file system based implementation */

typedef struct md_store_fs_t md_store_fs_t;
struct md_store_fs_t {
    md_store_t s;
    
    apr_pool_t *p;          /* duplicate for convenience */
    const char *base;       /* base directory of store */
};

#define FS_STORE(store)     (md_store_fs_t*)(((char*)store)-offsetof(md_store_fs_t, s))

static void fs_destroy(md_store_t *store);
static apr_status_t fs_load_mds(apr_array_header_t *mds, md_store_t *store, apr_pool_t *p);

static apr_status_t fs_load_value(void **pvalue, md_store_t *store, const char *name, 
                                  md_store_vtype_t vtype, apr_pool_t *p);
static apr_status_t fs_save_value(md_store_t *store, const char *name, 
                                  md_store_vtype_t vtype, void *value, int create);
static apr_status_t fs_remove_value(md_store_t *store, const char *name, 
                                    md_store_vtype_t vtype, int force);


apr_status_t md_store_fs_init(md_store_t **pstore, apr_pool_t *p, const char *path)
{
    md_store_fs_t *s_fs;
    apr_status_t rv = APR_SUCCESS;
    
    s_fs = apr_pcalloc(p, sizeof(*s_fs));
    s_fs->p = s_fs->s.p = p;
    s_fs->s.destroy = fs_destroy;
    s_fs->s.load_mds = fs_load_mds;

    s_fs->s.load_value = fs_load_value;
    s_fs->s.save_value= fs_save_value;
    s_fs->s.remove_value = fs_remove_value;

    s_fs->base = apr_pstrdup(p, path);
    
    if (APR_SUCCESS != (rv = md_util_is_dir(s_fs->base, p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, s_fs->p, "init fs store at %s", path);
    }
    *pstore = (rv == APR_SUCCESS)? &(s_fs->s) : NULL;
    return rv;
}

static void fs_destroy(md_store_t *store)
{
    md_store_fs_t *s_fs = FS_STORE(store);
    s_fs->s.p = NULL;
}

static apr_status_t pfs_md_readf(md_t **pmd, const char *fpath, apr_pool_t *p, apr_pool_t *ptemp)
{
    md_json_t *json;
    apr_status_t rv;
    
    *pmd = NULL;
    rv = md_json_readf(&json, ptemp, fpath);
    if (APR_SUCCESS == rv) {
        md_t *md = md_from_json(json, p);
        md->defn_name = apr_pstrdup(p, fpath);
        *pmd = md;
        return APR_SUCCESS;
    }
    return rv;
}

static apr_status_t pfs_md_writef(md_t *md, const char *dir, const char *name, apr_pool_t *p,
                                  int create)
{
    const char *fpath;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = apr_dir_make_recursive(dir, MD_FPROT_D_UONLY, p))) {
        if (APR_SUCCESS == (rv = md_util_path_merge(&fpath, p, dir, name, NULL))) {
            md_json_t *json = md_to_json(md, p);
            return (create? md_json_fcreatex(json, p, MD_JSON_FMT_INDENT, fpath)
                    : md_json_freplace(json, p, MD_JSON_FMT_INDENT, fpath));
        }
    }
    return rv;
}

#define FS_DN_DOMAINS      "domains"
#define FS_FN_MD_JSON      "md.json"
#define FS_FN_CERT_PEM     "cert.pem"
#define FS_FN_PKEY_PEM     "privkey.pem"
#define FS_FN_CHAIN_PEM    "chain.pem"

static const char *VTYPE_FNAME[] = {
    FS_FN_MD_JSON,
    FS_FN_CERT_PEM,
    FS_FN_PKEY_PEM,
    FS_FN_CHAIN_PEM,
};

static const char *vtype_filename(int vtype)
{
    if (vtype < sizeof(VTYPE_FNAME)/sizeof(VTYPE_FNAME[0])) {
        return VTYPE_FNAME[vtype];
    }
    return "UNKNOWN";
}

static apr_status_t pfs_load_value(void *baton, apr_pool_t *p, apr_pool_t *ptemp, va_list ap)
{
    md_store_fs_t *s_fs = baton;
    const char *fpath, *name, *filename;
    md_store_vtype_t vtype;
    void **pvalue;
    apr_status_t rv;
    
    pvalue= va_arg(ap, void **);
    name = va_arg(ap, const char *);
    vtype = va_arg(ap, int);    
    filename = vtype_filename(vtype);
    
    rv = md_util_path_merge(&fpath, ptemp, s_fs->base, FS_DN_DOMAINS, name, filename, NULL);
    if (APR_SUCCESS == rv) {
        if (pvalue != NULL) {
            switch (vtype) {
                case MD_STORE_V_MD:
                    rv = pfs_md_readf((md_t **)pvalue, fpath, p, ptemp);
                    break;
                case MD_STORE_V_CERT:
                    rv = md_cert_load((md_cert_t **)pvalue, p, fpath);
                    break;
                case MD_STORE_V_PKEY:
                    rv = md_pkey_load((md_pkey_t **)pvalue, p, fpath);
                    break;
                case MD_STORE_V_CHAIN:
                    rv = md_cert_load_chain((apr_array_header_t **)pvalue, p, fpath);
                    break;
                default:
                    return APR_ENOTIMPL;
            }
        }
        else { /* check for existence only */
            rv = md_util_is_file(fpath, ptemp);
        }
    }
    return rv;
}

static apr_status_t pfs_save_value(void *baton, apr_pool_t *p, apr_pool_t *ptemp, va_list ap)
{
    md_store_fs_t *s_fs = baton;
    const char *dir, *fpath, *name, *filename;
    md_store_vtype_t vtype;
    void *value;
    int create;
    apr_status_t rv;
    
    name = va_arg(ap, const char*);
    vtype = va_arg(ap, int);
    value = va_arg(ap, md_t *);
    create = va_arg(ap, int);
    filename = vtype_filename(vtype);
    
    if (APR_SUCCESS == (rv = md_util_path_merge(&dir, ptemp, s_fs->base, FS_DN_DOMAINS, name, NULL))
        && APR_SUCCESS == (rv = md_util_path_merge(&fpath, ptemp, dir, filename, NULL))) {
        switch (vtype) {
            case MD_STORE_V_MD:
                rv = pfs_md_writef((md_t*)value, dir, FS_FN_MD_JSON, ptemp, create);
                break;
            case MD_STORE_V_CERT:
                rv = md_cert_save((md_cert_t *)value, ptemp, fpath);
                break;
            case MD_STORE_V_PKEY:
                rv = md_pkey_save((md_pkey_t *)value, ptemp, fpath);
                break;
            case MD_STORE_V_CHAIN:
                rv = md_cert_save_chain((apr_array_header_t*)value, ptemp, fpath);
                break;
            default:
                return APR_ENOTIMPL;
        }
    }
    return rv;
}

static apr_status_t pfs_remove_value(void *baton, apr_pool_t *p, apr_pool_t *ptemp, va_list ap)
{
    md_store_fs_t *s_fs = baton;
    const char *dir, *name, *fpath, *filename;
    apr_status_t rv;
    int force;
    apr_finfo_t info;
    md_store_vtype_t vtype;
    
    name = va_arg(ap, const char*);
    vtype = va_arg(ap, int);
    force = va_arg(ap, int);
    filename = vtype_filename(vtype);
    
    if (APR_SUCCESS == (rv = md_util_path_merge(&dir, ptemp, s_fs->base, FS_DN_DOMAINS, name, NULL))
        && APR_SUCCESS == (rv = md_util_path_merge(&fpath, ptemp, dir, filename, NULL))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, ptemp, "start remove of md %s", name);

        if (APR_SUCCESS != (rv = apr_stat(&info, dir, APR_FINFO_TYPE, ptemp))) {
            if (APR_ENOENT == rv && force) {
                return APR_SUCCESS;
            }
            return rv;
        }
    
        switch (vtype) {
            case MD_STORE_V_MD:
                switch (info.filetype) {
                    case APR_DIR: /* how it should be */
                        /* TODO: check if there is important data, such as keys or certificates. 
                         * Only remove the md when forced in such cases. */
                        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, ptemp, "remove tree: %s", dir);
                        rv = md_util_ftree_remove(dir, ptemp);
                        break;
                    default:      /* how did that get here? suspicious */
                        if (!force) {
                            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, ptemp, 
                                          "remove md %s: not a directory at %s", name, dir);
                            return APR_EINVAL;
                        }
                        rv = apr_file_remove(dir, ptemp);
                        break;
                }
                break;
            case MD_STORE_V_CERT:
            case MD_STORE_V_PKEY:
            case MD_STORE_V_CHAIN:
                rv = apr_file_remove(fpath, ptemp);
                if (APR_ENOENT == rv && force) {
                    rv = APR_SUCCESS;
                }
                break;
            default:
                return APR_ENOTIMPL;
        }
    }
    return rv;
}

typedef struct {
    md_store_fs_t *s_fs;
    apr_array_header_t *mds;
} md_load_ctx;

static apr_status_t add_md(void *baton, apr_pool_t *p, apr_pool_t *ptemp, 
                           const char *dir, const char *name, apr_filetype_e ftype)
{
    md_load_ctx *ctx = baton;
    const char *fpath;
    apr_status_t rv;
    md_t *md, **pmd;
    
    rv = md_util_path_merge(&fpath, ptemp, dir, name, NULL);
    if (APR_SUCCESS == rv) {
        rv = pfs_md_readf(&md, fpath, p, ptemp);
        if (APR_SUCCESS == rv) {
            if (!md->name) {
                md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, ptemp, 
                              "md has no name, ignoring %s", fpath);
                return APR_EINVAL;
            }
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, ptemp, 
                          "adding md %s from %s", md->name, fpath);
            pmd = (md_t **)apr_array_push(ctx->mds);
            *pmd = md;
            return APR_SUCCESS;
        }
    }
    md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, ptemp, 
                  "reading md from: %s/%s", dir, name);
    return rv;
}

static int md_name_cmp(const void *v1, const void *v2)
{
    return - strcmp(((const md_t*)v1)->name, ((const md_t*)v2)->name);
}

static apr_status_t fs_load_mds(apr_array_header_t *mds, md_store_t *store, apr_pool_t *p)
{
    md_store_fs_t *s_fs = FS_STORE(store);
    md_load_ctx ctx;
    apr_status_t rv;
    
    ctx.s_fs = s_fs;
    ctx.mds = mds;
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, s_fs->p, "loading all mds in %s", s_fs->base);
    
    rv = md_util_files_do(add_md, &ctx, p, s_fs->base, FS_DN_DOMAINS, "*", FS_FN_MD_JSON, NULL);
    if (APR_SUCCESS == rv) {
        qsort(mds->elts, mds->nelts, sizeof(md_t *), md_name_cmp);
    }
    return rv;                        
}

static apr_status_t fs_load_value(void **pvalue, md_store_t *store, const char *name, 
                                  md_store_vtype_t vtype, apr_pool_t *p)
{
    md_store_fs_t *s_fs = FS_STORE(store);
    return md_util_pool_vdo(pfs_load_value, s_fs, p, pvalue, name, vtype, NULL);
}

static apr_status_t fs_save_value(md_store_t *store, const char *name, md_store_vtype_t vtype,
                                  void *value, int create)
{
    md_store_fs_t *s_fs = FS_STORE(store);
    return md_util_pool_vdo(pfs_save_value, s_fs, s_fs->p, name, vtype, value, create, NULL);
}

static apr_status_t fs_remove_value(md_store_t *store, const char *name, 
                                    md_store_vtype_t vtype, int force)
{
    md_store_fs_t *s_fs = FS_STORE(store);
    return md_util_pool_vdo(pfs_remove_value, s_fs, s_fs->p, name, vtype, force, NULL);
}
