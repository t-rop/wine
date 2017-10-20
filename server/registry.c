/*
 * Server-side registry management
 *
 * Copyright (C) 1999 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* To do:
 * - symbolic links
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"
#include "process.h"
#include "unicode.h"
#include "security.h"

#include "winternl.h"
#include "wine/library.h"

struct notify
{
    struct list       entry;    /* entry in list of notifications */
    struct event     *event;    /* event to set when changing this key */
    int               subtree;  /* true if subtree notification */
    unsigned int      filter;   /* which events to notify on */
    obj_handle_t      hkey;     /* hkey associated with this notification */
    struct process   *process;  /* process in which the hkey is valid */
};

/* a registry key */
struct key
{
    struct object     obj;         /* object header */
    WCHAR            *class;       /* key class */
    unsigned short    classlen;    /* length of class name */
    struct key       *parent;      /* parent key */
    int               last_subkey; /* last in use subkey */
    int               nb_subkeys;  /* count of allocated subkeys */
    struct key      **subkeys;     /* subkeys array */
    int               last_value;  /* last in use value */
    int               nb_values;   /* count of allocated values in array */
    struct key_value *values;      /* values array */
    unsigned int      flags;       /* flags */
    timeout_t         modif;       /* last modification time */
    struct list       notify_list; /* list of notifications */
};

/* key flags */
#define KEY_VOLATILE 0x0001  /* key is volatile (not saved to disk) */
#define KEY_DELETED  0x0002  /* key has been deleted */
#define KEY_DIRTY    0x0004  /* key has been modified */
#define KEY_SYMLINK  0x0008  /* key is a symbolic link */
#define KEY_WOW64    0x0010  /* key contains a Wow6432Node subkey */
#define KEY_WOWSHARE 0x0020  /* key is a Wow64 shared key (used for Software\Classes) */

/* a key value */
struct key_value
{
    WCHAR            *name;    /* value name */
    unsigned short    namelen; /* length of value name */
    unsigned int      type;    /* value type */
    data_size_t       len;     /* value data length in bytes */
    void             *data;    /* pointer to value data */
};

#define MIN_SUBKEYS  8   /* min. number of allocated subkeys per key */
#define MIN_VALUES   8   /* min. number of allocated values per key */

#define MAX_NAME_LEN  256    /* max. length of a key name */
#define MAX_VALUE_LEN 16383  /* max. length of a value name */

/* internal attributes flag */
#define WINE_OBJ_WOW64  0x80000000  /* use WoW64 redirection */

/* the root of the registry tree */
static struct key *root_key;

static const timeout_t ticks_1601_to_1970 = (timeout_t)86400 * (369 * 365 + 89) * TICKS_PER_SEC;
static const timeout_t save_period = 30 * -TICKS_PER_SEC;  /* delay between periodic saves */
static struct timeout_user *save_timeout_user;  /* saving timer */
static enum prefix_type { PREFIX_UNKNOWN, PREFIX_32BIT, PREFIX_64BIT } prefix_type;

static const WCHAR root_name[] = { '\\','R','E','G','I','S','T','R','Y' };
static const WCHAR wow6432node[] = {'W','o','w','6','4','3','2','N','o','d','e'};
static const WCHAR symlink_value[] = {'S','y','m','b','o','l','i','c','L','i','n','k','V','a','l','u','e'};
static const struct unicode_str root_name_str = { root_name, sizeof(root_name) };
static const struct unicode_str wow6432node_str = { wow6432node, sizeof(wow6432node) };
static const struct unicode_str symlink_str = { symlink_value, sizeof(symlink_value) };

static void set_periodic_save_timer(void);
static struct key_value *find_value( const struct key *key, const struct unicode_str *name, int *index );

/* information about where to save a registry branch */
struct save_branch_info
{
    struct key  *key;
    const char  *path;
};

#define MAX_SAVE_BRANCH_INFO 3
static int save_branch_count;
static struct save_branch_info save_branch_info[MAX_SAVE_BRANCH_INFO];


/* information about a file being loaded */
struct file_load_info
{
    const char *filename; /* input file name */
    FILE       *file;     /* input file */
    char       *buffer;   /* line buffer */
    int         len;      /* buffer length */
    int         line;     /* current input line */
    WCHAR      *tmp;      /* temp buffer to use while parsing input */
    size_t      tmplen;   /* length of temp buffer */
};


static void key_dump( struct object *obj, int verbose );
static struct object_type *key_get_type( struct object *obj );
static int key_link_name( struct object *obj, struct object_name *name, struct object *parent );
static void key_unlink_name( struct object *obj, struct object_name *name );
static struct object *key_lookup_name( struct object *obj, struct unicode_str *name, unsigned int attr );
static unsigned int key_map_access( struct object *obj, unsigned int access );
static struct security_descriptor *key_get_sd( struct object *obj );
static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void key_destroy( struct object *obj );

static const struct object_ops key_ops =
{
    sizeof(struct key),      /* size */
    key_dump,                /* dump */
    key_get_type,            /* get_type */
    no_add_queue,            /* add_queue */
    NULL,                    /* remove_queue */
    NULL,                    /* signaled */
    NULL,                    /* satisfied */
    no_signal,               /* signal */
    no_get_fd,               /* get_fd */
    key_map_access,          /* map_access */
    key_get_sd,              /* get_sd */
    default_set_sd,          /* set_sd */
    key_lookup_name,         /* lookup_name */
    key_link_name,           /* link_name */
    key_unlink_name,         /* unlink_name */
    no_open_file,            /* open_file */
    key_close_handle,        /* close_handle */
    key_destroy              /* destroy */
};


static inline int is_wow6432node( const WCHAR *name, unsigned int len )
{
    return (len == sizeof(wow6432node) &&
            !memicmpW( name, wow6432node, sizeof(wow6432node)/sizeof(WCHAR) ));
}

/*
 * The registry text file format v2 used by this code is similar to the one
 * used by REGEDIT import/export functionality, with the following differences:
 * - strings and key names can contain \x escapes for Unicode
 * - key names use escapes too in order to support Unicode
 * - the modification time optionally follows the key name
 * - REG_EXPAND_SZ and REG_MULTI_SZ are saved as strings instead of hex
 */

/* dump the full path of a key */
static void dump_path( const struct key *key, const struct key *base, FILE *f )
{
    if (key->parent && key->parent != base)
    {
        dump_path( key->parent, base, f );
        fprintf( f, "\\\\" );
    }
    dump_strW( key->obj.name->name, key->obj.name->len / sizeof(WCHAR), f, "[]" );
}

/* dump a value to a text file */
static void dump_value( const struct key_value *value, FILE *f )
{
    unsigned int i, dw;
    int count;

    if (value->namelen)
    {
        fputc( '\"', f );
        count = 1 + dump_strW( value->name, value->namelen / sizeof(WCHAR), f, "\"\"" );
        count += fprintf( f, "\"=" );
    }
    else count = fprintf( f, "@=" );

    switch(value->type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:
        /* only output properly terminated strings in string format */
        if (value->len < sizeof(WCHAR)) break;
        if (value->len % sizeof(WCHAR)) break;
        if (((WCHAR *)value->data)[value->len / sizeof(WCHAR) - 1]) break;
        if (value->type != REG_SZ) fprintf( f, "str(%x):", value->type );
        fputc( '\"', f );
        dump_strW( (WCHAR *)value->data, value->len / sizeof(WCHAR), f, "\"\"" );
        fprintf( f, "\"\n" );
        return;

    case REG_DWORD:
        if (value->len != sizeof(dw)) break;
        memcpy( &dw, value->data, sizeof(dw) );
        fprintf( f, "dword:%08x\n", dw );
        return;
    }

    if (value->type == REG_BINARY) count += fprintf( f, "hex:" );
    else count += fprintf( f, "hex(%x):", value->type );
    for (i = 0; i < value->len; i++)
    {
        count += fprintf( f, "%02x", *((unsigned char *)value->data + i) );
        if (i < value->len-1)
        {
            fputc( ',', f );
            if (++count > 76)
            {
                fprintf( f, "\\\n  " );
                count = 2;
            }
        }
    }
    fputc( '\n', f );
}

/* save a registry and all its subkeys to a text file */
static void save_subkeys( const struct key *key, const struct key *base, FILE *f )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    /* save key if it has either some values or no subkeys, or needs special options */
    /* keys with no values but subkeys are saved implicitly by saving the subkeys */
    if ((key->last_value >= 0) || (key->last_subkey == -1) || key->class || (key->flags & KEY_SYMLINK))
    {
        fprintf( f, "\n[" );
        if (key != base) dump_path( key, base, f );
        fprintf( f, "] %u\n", (unsigned int)((key->modif - ticks_1601_to_1970) / TICKS_PER_SEC) );
        fprintf( f, "#time=%x%08x\n", (unsigned int)(key->modif >> 32), (unsigned int)key->modif );
        if (key->class)
        {
            fprintf( f, "#class=\"" );
            dump_strW( key->class, key->classlen / sizeof(WCHAR), f, "\"\"" );
            fprintf( f, "\"\n" );
        }
        if (key->flags & KEY_SYMLINK) fputs( "#link\n", f );
        for (i = 0; i <= key->last_value; i++) dump_value( &key->values[i], f );
    }
    for (i = 0; i <= key->last_subkey; i++) save_subkeys( key->subkeys[i], base, f );
}

static void dump_operation( const struct key *key, const struct key_value *value, const char *op )
{
    fprintf( stderr, "%s key ", op );
    if (key) dump_path( key, NULL, stderr );
    else fprintf( stderr, "ERROR" );
    if (value)
    {
        fprintf( stderr, " value ");
        dump_value( value, stderr );
    }
    else fprintf( stderr, "\n" );
}

static void key_dump( struct object *obj, int verbose )
{
    struct key *key = (struct key *)obj;
    assert( obj->ops == &key_ops );
    fprintf( stderr, "Key flags=%x\n", key->flags );
}

static struct object_type *key_get_type( struct object *obj )
{
    static const WCHAR name[] = {'K','e','y'};
    static const struct unicode_str str = { name, sizeof(name) };
    return get_object_type( &str );
}

static struct key *find_subkey( const struct key *key, const WCHAR *name, int len, int *index );
static int grow_subkeys( struct key *key );
static void touch_key( struct key *key, unsigned int change );

static int key_link_name( struct object *obj, struct object_name *name, struct object *parent )
{
    static const WCHAR registryW[] = {'R','E','G','I','S','T','R','Y'};
    struct key *key = (struct key *)obj;
    struct key *key_parent = (struct key *)parent;
    struct object *root_directory = get_root_directory();
    int index, i;

    /* are we creating the root key? */
    if (parent == root_directory && !strncmpiW( registryW, name->name, name->len/sizeof(WCHAR) ))
    {
        key->parent = NULL;
        directory_link_name( obj, name, parent );
        release_object( root_directory );
        return 1;
    }
    release_object( root_directory );

    if (parent->ops != &key_ops)
    {
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return 0;
    }

    if (key_parent->last_subkey + 1 == key_parent->nb_subkeys)
    {
        /* need to grow the array */
        if (!grow_subkeys( key_parent )) return 0;
    }
    /* find the sorted index */
    assert( !find_subkey( key_parent, name->name, name->len, &index ) );
    for (i = ++key_parent->last_subkey; i > index; i--)
        key_parent->subkeys[i] = key_parent->subkeys[i-1];
    key_parent->subkeys[index] = key;

    touch_key( key_parent, REG_NOTIFY_CHANGE_NAME );

    key->parent = key_parent;

    name->parent = grab_object( parent );
    return 1;
}

static void key_unlink_name( struct object *obj, struct object_name *name )
{
    if ((struct key *)obj == root_key)
        default_unlink_name( obj, name );
}

/* follow a symlink and return the resolved key */
static struct key *follow_symlink( struct key *key, unsigned int attr )
{
    struct unicode_str path;
    struct key_value *value;
    int index;

    if (!(value = find_value( key, &symlink_str, &index ))) return NULL;

    path.str = value->data;
    path.len = (value->len / sizeof(WCHAR)) * sizeof(WCHAR);

    attr = (attr | OBJ_OPENLINK) & ~WINE_OBJ_WOW64;

    if (path.len && path.str[0] == '\\')
        return open_named_object( NULL, &key_ops, &path, attr );
    else /* relative symlink */
        return open_named_object( &key->parent->obj, &key_ops, &path, attr );
}

static struct unicode_str *get_path_token(const struct unicode_str *path, struct unicode_str *token );
static struct key *find_wow64_subkey( struct key *key, const struct unicode_str *name );

static struct object *key_lookup_name( struct object *obj, struct unicode_str *name,
                                       unsigned int attr )
{
    struct key *key = (struct key *)obj;
    struct key *found = NULL;
    struct unicode_str tmp;
    const WCHAR *p;

    assert( obj->ops == &key_ops );

    if (!name) return NULL;  /* open the key itself */

    if (!(p = memchrW( name->str, '\\', name->len / sizeof(WCHAR) )))
        /* Last element in the path name */
        tmp.len = name->len;
    else
        tmp.len = (p - name->str) * sizeof(WCHAR);

    tmp.str = name->str;
    if (!tmp.len) return NULL;
    if (tmp.len > MAX_NAME_LEN * sizeof(WCHAR))
    {
        set_error(STATUS_INVALID_PARAMETER);
        return NULL;
    }

    /* if this is the WOW6432Node subkey of a shared key, look in the 64-bit parent instead */
    if (is_wow6432node( key->obj.name->name, key->obj.name->len ) &&
        (key->parent->flags & KEY_WOWSHARE) && (attr & WINE_OBJ_WOW64))
        found = find_subkey( key->parent, tmp.str, tmp.len, NULL );
    else
        found = find_subkey( key, tmp.str, tmp.len, NULL );

    if (found)
    {
        /* Resolve symlinks */
        if (!(attr & OBJ_OPENLINK))
        {
            struct key *target;
            unsigned int iteration = 0;

            while (found->flags & KEY_SYMLINK)
            {
                if (!(target = follow_symlink( found, attr )))
                    break;

                release_object( found );

                if (iteration > 16)
                {
                    release_object( target );
                    set_error( STATUS_NAME_TOO_LONG );
                    return NULL;
                }
                found = target;
                iteration++;
            }
        }

        /* Move to the next element */
        while (tmp.len < name->len && *p == '\\')
        {
            p++;
            tmp.len += sizeof(WCHAR);
        }
        name->str = p;
        name->len -= tmp.len;

        /* Resolve WoW64 */
        if (attr & WINE_OBJ_WOW64)
        {
            struct unicode_str token = {0};
            get_path_token( name, &token );
            found = find_wow64_subkey( found, &token );

            /* don't return the WoW6432Node subkey of a shared key if it's the last element */
            if (is_wow6432node( found->obj.name->name, found->obj.name->len ) &&
                (found->parent->flags & KEY_WOWSHARE) && !name->len)
            {
                struct key *parent = (struct key *)grab_object( found->parent );
                release_object( found );
                found = parent;
            }
        }
        return &found->obj;
    }

    if (tmp.len < name->len)  /* Path still has elements */
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );

    return NULL;
}

/* notify waiter and maybe delete the notification */
static void do_notification( struct key *key, struct notify *notify, int del )
{
    if (notify->event)
    {
        set_event( notify->event );
        release_object( notify->event );
        notify->event = NULL;
    }
    if (del)
    {
        list_remove( &notify->entry );
        free( notify );
    }
}

static inline struct notify *find_notify( struct key *key, struct process *process, obj_handle_t hkey )
{
    struct notify *notify;

    LIST_FOR_EACH_ENTRY( notify, &key->notify_list, struct notify, entry )
    {
        if (notify->process == process && notify->hkey == hkey) return notify;
    }
    return NULL;
}

static unsigned int key_map_access( struct object *obj, unsigned int access )
{
    if (access & GENERIC_READ)    access |= KEY_READ;
    if (access & GENERIC_WRITE)   access |= KEY_WRITE;
    if (access & GENERIC_EXECUTE) access |= KEY_EXECUTE;
    if (access & GENERIC_ALL)     access |= KEY_ALL_ACCESS;
    /* filter the WOW64 masks, as they aren't real access bits */
    return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL |
                      KEY_WOW64_64KEY | KEY_WOW64_32KEY);
}

static struct security_descriptor *key_get_sd( struct object *obj )
{
    static struct security_descriptor *key_default_sd;

    if (obj->sd) return obj->sd;

    if (!key_default_sd)
    {
        size_t users_sid_len = security_sid_len( security_builtin_users_sid );
        size_t admins_sid_len = security_sid_len( security_builtin_admins_sid );
        size_t dacl_len = sizeof(ACL) + 2 * offsetof( ACCESS_ALLOWED_ACE, SidStart )
                          + users_sid_len + admins_sid_len;
        ACCESS_ALLOWED_ACE *aaa;
        ACL *dacl;

        key_default_sd = mem_alloc( sizeof(*key_default_sd) + 2 * admins_sid_len + dacl_len );
        key_default_sd->control   = SE_DACL_PRESENT;
        key_default_sd->owner_len = admins_sid_len;
        key_default_sd->group_len = admins_sid_len;
        key_default_sd->sacl_len  = 0;
        key_default_sd->dacl_len  = dacl_len;
        memcpy( key_default_sd + 1, security_builtin_admins_sid, admins_sid_len );
        memcpy( (char *)(key_default_sd + 1) + admins_sid_len, security_builtin_admins_sid, admins_sid_len );

        dacl = (ACL *)((char *)(key_default_sd + 1) + 2 * admins_sid_len);
        dacl->AclRevision = ACL_REVISION;
        dacl->Sbz1 = 0;
        dacl->AclSize = dacl_len;
        dacl->AceCount = 2;
        dacl->Sbz2 = 0;
        aaa = (ACCESS_ALLOWED_ACE *)(dacl + 1);
        aaa->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
        aaa->Header.AceFlags = INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE;
        aaa->Header.AceSize = offsetof( ACCESS_ALLOWED_ACE, SidStart ) + users_sid_len;
        aaa->Mask = GENERIC_READ;
        memcpy( &aaa->SidStart, security_builtin_users_sid, users_sid_len );
        aaa = (ACCESS_ALLOWED_ACE *)((char *)aaa + aaa->Header.AceSize);
        aaa->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
        aaa->Header.AceFlags = 0;
        aaa->Header.AceSize = offsetof( ACCESS_ALLOWED_ACE, SidStart ) + admins_sid_len;
        aaa->Mask = KEY_ALL_ACCESS;
        memcpy( &aaa->SidStart, security_builtin_admins_sid, admins_sid_len );
    }
    return key_default_sd;
}

/* close the notification associated with a handle */
static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct key * key = (struct key *) obj;
    struct notify *notify = find_notify( key, process, handle );
    if (notify) do_notification( key, notify, 1 );
    return 1;  /* ok to close */
}

static int delete_key( struct key *key, int recurse );

static void key_destroy( struct object *obj )
{
    int i;
    struct list *ptr;
    struct key *key = (struct key *)obj;
    assert( obj->ops == &key_ops );

    free( key->class );
    for (i = 0; i <= key->last_value; i++)
    {
        free( key->values[i].name );
        free( key->values[i].data );
    }
    free( key->values );
    for (i = 0; i <= key->last_subkey; i++)
    {
        key->subkeys[i]->parent = NULL;
        delete_key( key->subkeys[i], 1 );
    }
    free( key->subkeys );
    /* unconditionally notify everything waiting on this key */
    while ((ptr = list_head( &key->notify_list )))
    {
        struct notify *notify = LIST_ENTRY( ptr, struct notify, entry );
        do_notification( key, notify, 1 );
    }
}

/* return the next token in a given path */
/* token->str must point inside the path, or be NULL for the first call */
static struct unicode_str *get_path_token( const struct unicode_str *path, struct unicode_str *token )
{
    data_size_t i = 0, len = path->len / sizeof(WCHAR);

    if (!token->str)  /* first time */
    {
        /* path cannot start with a backslash */
        if (len && path->str[0] == '\\')
        {
            set_error( STATUS_OBJECT_PATH_INVALID );
            return NULL;
        }
    }
    else
    {
        i = token->str - path->str;
        i += token->len / sizeof(WCHAR);
        while (i < len && path->str[i] == '\\') i++;
    }
    token->str = path->str + i;
    while (i < len && path->str[i] != '\\') i++;
    token->len = (path->str + i - token->str) * sizeof(WCHAR);
    return token;
}

/* mark a key and all its parents as dirty (modified) */
static void make_dirty( struct key *key )
{
    while (key)
    {
        if (key->flags & (KEY_DIRTY|KEY_VOLATILE)) return;  /* nothing to do */
        key->flags |= KEY_DIRTY;
        key = key->parent;
    }
}

/* mark a key and all its subkeys as clean (not modified) */
static void make_clean( struct key *key )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    if (!(key->flags & KEY_DIRTY)) return;
    key->flags &= ~KEY_DIRTY;
    for (i = 0; i <= key->last_subkey; i++) make_clean( key->subkeys[i] );
}

/* go through all the notifications and send them if necessary */
static void check_notify( struct key *key, unsigned int change, int not_subtree )
{
    struct list *ptr, *next;

    LIST_FOR_EACH_SAFE( ptr, next, &key->notify_list )
    {
        struct notify *n = LIST_ENTRY( ptr, struct notify, entry );
        if ( ( not_subtree || n->subtree ) && ( change & n->filter ) )
            do_notification( key, n, 0 );
    }
}

/* update key modification time */
static void touch_key( struct key *key, unsigned int change )
{
    struct key *k;

    key->modif = current_time;
    make_dirty( key );

    /* do notifications */
    check_notify( key, change, 1 );
    for ( k = key->parent; k; k = k->parent )
        check_notify( k, change & ~REG_NOTIFY_CHANGE_LAST_SET, 0 );
}

/* try to grow the array of subkeys; return 1 if OK, 0 on error */
static int grow_subkeys( struct key *key )
{
    struct key **new_subkeys;
    int nb_subkeys;

    if (key->nb_subkeys)
    {
        nb_subkeys = key->nb_subkeys + (key->nb_subkeys / 2);  /* grow by 50% */
        if (!(new_subkeys = realloc( key->subkeys, nb_subkeys * sizeof(*new_subkeys) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = mem_alloc( nb_subkeys * sizeof(*new_subkeys) ))) return 0;
    }
    key->subkeys    = new_subkeys;
    key->nb_subkeys = nb_subkeys;
    return 1;
}

/* free a given subkey */
static void free_subkey( struct key *key )
{
    struct key *parent = key->parent;
    int i, nb_subkeys;

    if (!parent) return;

    /* remove from parent subkeys list */
    for (i = 0; i <= parent->last_subkey; i++)
        if (parent->subkeys[i] == key) break;
    assert( i <= parent->last_subkey );

    for (; i < parent->last_subkey; i++) parent->subkeys[i] = parent->subkeys[i + 1];
    parent->last_subkey--;
    key->flags |= KEY_DELETED;
    key->parent = NULL;

    /* try to shrink the array */
    nb_subkeys = parent->nb_subkeys;
    if (nb_subkeys > MIN_SUBKEYS && parent->last_subkey < nb_subkeys / 2)
    {
        struct key **new_subkeys;
        nb_subkeys -= nb_subkeys / 3;  /* shrink by 33% */
        if (nb_subkeys < MIN_SUBKEYS) nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = realloc( parent->subkeys, nb_subkeys * sizeof(*new_subkeys) ))) return;
        parent->subkeys = new_subkeys;
        parent->nb_subkeys = nb_subkeys;
    }
}

/* find the named child of a given key and return its index */
static struct key *find_subkey( const struct key *key, const WCHAR *name, int namelen, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_subkey;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->subkeys[i]->obj.name->len, namelen );
        res = memicmpW( key->subkeys[i]->obj.name->name, name, len / sizeof(WCHAR) );
        if (!res) res = key->subkeys[i]->obj.name->len - namelen;
        if (!res)
        {
            if (index) *index = i;
            return (struct key *)grab_object( key->subkeys[i] );
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    if (index) *index = min;  /* this is where we should insert it */
    return NULL;
}

/* return the wow64 variant of the key, or the key itself if none */
static struct key *find_wow64_subkey( struct key *key, const struct unicode_str *name )
{
    if (!(key->flags & KEY_WOW64)) return key;
    if (key->parent->flags & KEY_WOWSHARE)
    {
        /* look under the parent instead */
        struct key *wow64_key = find_subkey( key->parent, wow6432node, sizeof(wow6432node), NULL );
        struct key *subkey = find_subkey( wow64_key, key->obj.name->name, key->obj.name->len, NULL );
        release_object( key );
        release_object( wow64_key );
        key = subkey;
        assert( key );  /* if KEY_WOW64 is set we must find it */
    }
    else if (!is_wow6432node( name->str, name->len ))
    {
        struct key *subkey = find_subkey( key, wow6432node, sizeof(wow6432node), NULL );
        release_object( key );
        key = subkey;
        assert( key );  /* if KEY_WOW64 is set we must find it */
    }
    return key;
}

/* create a subkey */
static struct key *create_key( struct object *parent, const struct unicode_str *name,
                               const struct unicode_str *class, unsigned int options,
                               unsigned int access, unsigned int attributes,
                               const struct security_descriptor *sd )
{
    struct key *key;

    if (!(options & REG_OPTION_CREATE_LINK))
        attributes |= OBJ_OPENIF;

    if (!name || !name->len)
    {
        if (!parent)
            set_error( STATUS_OBJECT_PATH_SYNTAX_BAD );
        else
            grab_object( parent );
        return (struct key *)parent;
    }

    if ((key = create_named_object( parent, &key_ops, name, attributes, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            /* initialize it if it didn't already exist */
            if (class && class->len)
            {
                key->classlen = class->len;
                if (!(key->class = memdup( class->str, key->classlen ))) key->classlen = 0;
            }
            else
            {
                key->class = NULL;
                key->classlen = 0;
            }
            key->flags       = options & REG_OPTION_VOLATILE ? KEY_VOLATILE : KEY_DIRTY;
            if (options & REG_OPTION_CREATE_LINK) key->flags |= KEY_SYMLINK;
            key->last_subkey = -1;
            key->nb_subkeys  = 0;
            key->subkeys     = NULL;
            key->nb_values   = 0;
            key->last_value  = -1;
            key->values      = NULL;
            key->modif       = current_time;
            list_init( &key->notify_list );

            if ((key->parent && key->parent->flags & KEY_VOLATILE) && !(options & REG_OPTION_VOLATILE))
            {
                set_error( STATUS_CHILD_MUST_BE_VOLATILE );
                free_subkey( key );
                release_object( key );
                return NULL;
            }

            if (debug_level > 1) dump_operation( key, NULL, "Create" );
            /* keys are persistent */
            grab_object( key );
        }
    }

    return key;
}

/* recursively create a subkey (for internal use only) */
static struct key *create_key_recursive( struct key *key, const struct unicode_str *name, timeout_t modif )
{
    struct key *root = key;
    struct unicode_str token;

    token.str = NULL;
    if (!get_path_token( name, &token )) return NULL;
    while (token.len)
    {
        struct key *subkey;
        if (!(subkey = create_key( &key->obj, &token, NULL, 0, 0, OBJ_OPENIF, NULL ))) break;
        if (key != root) release_object( key );
        key = subkey;
        get_path_token( name, &token );
    }

    return key;
}

/* query information about a key or a subkey */
static void enum_key( const struct key *key, int index, int info_class,
                      struct enum_key_reply *reply )
{
    static const WCHAR backslash[] = { '\\' };
    int i;
    data_size_t len, namelen, classlen;
    data_size_t max_subkey = 0, max_class = 0;
    data_size_t max_value = 0, max_data = 0;
    const struct key *k;
    char *data;

    if (index != -1)  /* -1 means use the specified key directly */
    {
        if ((index < 0) || (index > key->last_subkey))
        {
            set_error( STATUS_NO_MORE_ENTRIES );
            return;
        }
        key = key->subkeys[index];
    }

    namelen = key->obj.name->len;
    classlen = key->classlen;

    switch(info_class)
    {
    case KeyNameInformation:
        namelen = 0;
        for (k = key; k != root_key; k = k->parent)
            namelen += k->obj.name->len + sizeof(backslash);
        if (!namelen) return;
        namelen += sizeof(root_name);
        /* fall through */
    case KeyBasicInformation:
        classlen = 0; /* only return the name */
        /* fall through */
    case KeyNodeInformation:
        reply->max_subkey = 0;
        reply->max_class  = 0;
        reply->max_value  = 0;
        reply->max_data   = 0;
        break;
    case KeyFullInformation:
    case KeyCachedInformation:
        for (i = 0; i <= key->last_subkey; i++)
        {
            if (key->subkeys[i]->obj.name->len > max_subkey) max_subkey = key->subkeys[i]->obj.name->len;
            if (key->subkeys[i]->classlen > max_class) max_class = key->subkeys[i]->classlen;
        }
        for (i = 0; i <= key->last_value; i++)
        {
            if (key->values[i].namelen > max_value) max_value = key->values[i].namelen;
            if (key->values[i].len > max_data) max_data = key->values[i].len;
        }
        reply->max_subkey = max_subkey;
        reply->max_class  = max_class;
        reply->max_value  = max_value;
        reply->max_data   = max_data;
        reply->namelen    = namelen;
        if (info_class == KeyCachedInformation)
            classlen = 0; /* don't return any data, only its size */
        namelen = 0;  /* don't return name */
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    reply->subkeys = key->last_subkey + 1;
    reply->values  = key->last_value + 1;
    reply->modif   = key->modif;
    reply->total   = namelen + classlen;

    len = min( reply->total, get_reply_max_size() );
    if (len && (data = set_reply_data_size( len )))
    {
        if (len > namelen)
        {
            reply->namelen = namelen;
            memcpy( data, key->obj.name->name, namelen );
            memcpy( data + namelen, key->class, len - namelen );
        }
        else if (info_class == KeyNameInformation)
        {
            data_size_t pos = namelen;
            reply->namelen = namelen;
            for (k = key; k != root_key; k = k->parent)
            {
                pos -= k->obj.name->len;
                if (pos < len) memcpy( data + pos, k->obj.name->name,
                                       min( k->obj.name->len, len - pos ) );
                pos -= sizeof(backslash);
                if (pos < len) memcpy( data + pos, backslash,
                                       min( sizeof(backslash), len - pos ) );
            }
            memcpy( data, root_name, min( sizeof(root_name), len ) );
        }
        else
        {
            reply->namelen = len;
            memcpy( data, key->obj.name->name, len );
        }
    }
    if (debug_level > 1) dump_operation( key, NULL, "Enum" );
}

/* delete a key and its values */
static int delete_key( struct key *key, int recurse )
{
    struct key *parent = key->parent;

    while (recurse && (key->last_subkey>=0))
        if (0 > delete_key(key->subkeys[key->last_subkey], 1))
            return -1;

    /* we can only delete a key that has no subkeys */
    if (key->last_subkey >= 0)
    {
        set_error( STATUS_ACCESS_DENIED );
        return -1;
    }

    if (debug_level > 1) dump_operation( key, NULL, "Delete" );

    /* remove from parent's subkey list */
    free_subkey( key );

    /* release the persistent reference */
    release_object( key );

    if (parent)
        touch_key( parent, REG_NOTIFY_CHANGE_NAME );
    return 0;
}

/* try to grow the array of values; return 1 if OK, 0 on error */
static int grow_values( struct key *key )
{
    struct key_value *new_val;
    int nb_values;

    if (key->nb_values)
    {
        nb_values = key->nb_values + (key->nb_values / 2);  /* grow by 50% */
        if (!(new_val = realloc( key->values, nb_values * sizeof(*new_val) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_values = MIN_VALUES;
        if (!(new_val = mem_alloc( nb_values * sizeof(*new_val) ))) return 0;
    }
    key->values = new_val;
    key->nb_values = nb_values;
    return 1;
}

/* find the named value of a given key and return its index in the array */
static struct key_value *find_value( const struct key *key, const struct unicode_str *name, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_value;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->values[i].namelen, name->len );
        res = memicmpW( key->values[i].name, name->str, len / sizeof(WCHAR) );
        if (!res) res = key->values[i].namelen - name->len;
        if (!res)
        {
            *index = i;
            return &key->values[i];
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    *index = min;  /* this is where we should insert it */
    return NULL;
}

/* insert a new value; the index must have been returned by find_value */
static struct key_value *insert_value( struct key *key, const struct unicode_str *name, int index )
{
    struct key_value *value;
    WCHAR *new_name = NULL;
    int i;

    if (name->len > MAX_VALUE_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_NAME_TOO_LONG );
        return NULL;
    }
    if (key->last_value + 1 == key->nb_values)
    {
        if (!grow_values( key )) return NULL;
    }
    if (name->len && !(new_name = memdup( name->str, name->len ))) return NULL;
    for (i = ++key->last_value; i > index; i--) key->values[i] = key->values[i - 1];
    value = &key->values[index];
    value->name    = new_name;
    value->namelen = name->len;
    value->len     = 0;
    value->data    = NULL;
    return value;
}

/* set a key value */
static void set_value( struct key *key, const struct unicode_str *name,
                       int type, const void *data, data_size_t len )
{
    struct key_value *value;
    void *ptr = NULL;
    int index;

    if ((value = find_value( key, name, &index )))
    {
        /* check if the new value is identical to the existing one */
        if (value->type == type && value->len == len &&
            value->data && !memcmp( value->data, data, len ))
        {
            if (debug_level > 1) dump_operation( key, value, "Skip setting" );
            return;
        }
    }

    if (key->flags & KEY_SYMLINK)
    {
        if (type != REG_LINK || name->len != symlink_str.len ||
            memicmpW( name->str, symlink_str.str, name->len / sizeof(WCHAR) ))
        {
            set_error( STATUS_ACCESS_DENIED );
            return;
        }
    }

    if (len && !(ptr = memdup( data, len ))) return;

    if (!value)
    {
        if (!(value = insert_value( key, name, index )))
        {
            free( ptr );
            return;
        }
    }
    else free( value->data ); /* already existing, free previous data */

    value->type  = type;
    value->len   = len;
    value->data  = ptr;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );
    if (debug_level > 1) dump_operation( key, value, "Set" );
}

/* get a key value */
static void get_value( struct key *key, const struct unicode_str *name, int *type, data_size_t *len )
{
    struct key_value *value;
    int index;

    if ((value = find_value( key, name, &index )))
    {
        *type = value->type;
        *len  = value->len;
        if (value->data) set_reply_data( value->data, min( value->len, get_reply_max_size() ));
        if (debug_level > 1) dump_operation( key, value, "Get" );
    }
    else
    {
        *type = -1;
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
    }
}

/* enumerate a key value */
static void enum_value( struct key *key, int i, int info_class, struct enum_key_value_reply *reply )
{
    struct key_value *value;

    if (i < 0 || i > key->last_value) set_error( STATUS_NO_MORE_ENTRIES );
    else
    {
        void *data;
        data_size_t namelen, maxlen;

        value = &key->values[i];
        reply->type = value->type;
        namelen = value->namelen;

        switch(info_class)
        {
        case KeyValueBasicInformation:
            reply->total = namelen;
            break;
        case KeyValueFullInformation:
            reply->total = namelen + value->len;
            break;
        case KeyValuePartialInformation:
            reply->total = value->len;
            namelen = 0;
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }

        maxlen = min( reply->total, get_reply_max_size() );
        if (maxlen && ((data = set_reply_data_size( maxlen ))))
        {
            if (maxlen > namelen)
            {
                reply->namelen = namelen;
                memcpy( data, value->name, namelen );
                memcpy( (char *)data + namelen, value->data, maxlen - namelen );
            }
            else
            {
                reply->namelen = maxlen;
                memcpy( data, value->name, maxlen );
            }
        }
        if (debug_level > 1) dump_operation( key, value, "Enum" );
    }
}

/* delete a value */
static void delete_value( struct key *key, const struct unicode_str *name )
{
    struct key_value *value;
    int i, index, nb_values;

    if (!(value = find_value( key, name, &index )))
    {
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return;
    }
    if (debug_level > 1) dump_operation( key, value, "Delete" );
    free( value->name );
    free( value->data );
    for (i = index; i < key->last_value; i++) key->values[i] = key->values[i + 1];
    key->last_value--;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );

    /* try to shrink the array */
    nb_values = key->nb_values;
    if (nb_values > MIN_VALUES && key->last_value < nb_values / 2)
    {
        struct key_value *new_val;
        nb_values -= nb_values / 3;  /* shrink by 33% */
        if (nb_values < MIN_VALUES) nb_values = MIN_VALUES;
        if (!(new_val = realloc( key->values, nb_values * sizeof(*new_val) ))) return;
        key->values = new_val;
        key->nb_values = nb_values;
    }
}

/* get the registry key corresponding to an hkey handle */
static struct key *get_hkey_obj( obj_handle_t hkey, unsigned int access )
{
    struct key *key = (struct key *)get_handle_obj( current->process, hkey, access, &key_ops );

    if (key && key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        release_object( key );
        key = NULL;
    }
    return key;
}

/* read a line from the input file */
static int read_next_line( struct file_load_info *info )
{
    char *newbuf;
    int newlen, pos = 0;

    info->line++;
    for (;;)
    {
        if (!fgets( info->buffer + pos, info->len - pos, info->file ))
            return (pos != 0);  /* EOF */
        pos = strlen(info->buffer);
        if (info->buffer[pos-1] == '\n')
        {
            /* got a full line */
            info->buffer[--pos] = 0;
            if (pos > 0 && info->buffer[pos-1] == '\r') info->buffer[pos-1] = 0;
            return 1;
        }
        if (pos < info->len - 1) return 1;  /* EOF but something was read */

        /* need to enlarge the buffer */
        newlen = info->len + info->len / 2;
        if (!(newbuf = realloc( info->buffer, newlen )))
        {
            set_error( STATUS_NO_MEMORY );
            return -1;
        }
        info->buffer = newbuf;
        info->len = newlen;
    }
}

/* make sure the temp buffer holds enough space */
static int get_file_tmp_space( struct file_load_info *info, size_t size )
{
    WCHAR *tmp;
    if (info->tmplen >= size) return 1;
    if (!(tmp = realloc( info->tmp, size )))
    {
        set_error( STATUS_NO_MEMORY );
        return 0;
    }
    info->tmp = tmp;
    info->tmplen = size;
    return 1;
}

/* report an error while loading an input file */
static void file_read_error( const char *err, struct file_load_info *info )
{
    if (info->filename)
        fprintf( stderr, "%s:%d: %s '%s'\n", info->filename, info->line, err, info->buffer );
    else
        fprintf( stderr, "<fd>:%d: %s '%s'\n", info->line, err, info->buffer );
}

/* convert a data type tag to a value type */
static int get_data_type( const char *buffer, int *type, int *parse_type )
{
    struct data_type { const char *tag; int len; int type; int parse_type; };

    static const struct data_type data_types[] =
    {                   /* actual type */  /* type to assume for parsing */
        { "\"",        1,   REG_SZ,              REG_SZ },
        { "str:\"",    5,   REG_SZ,              REG_SZ },
        { "str(2):\"", 8,   REG_EXPAND_SZ,       REG_SZ },
        { "str(7):\"", 8,   REG_MULTI_SZ,        REG_SZ },
        { "hex:",      4,   REG_BINARY,          REG_BINARY },
        { "dword:",    6,   REG_DWORD,           REG_DWORD },
        { "hex(",      4,   -1,                  REG_BINARY },
        { NULL,        0,    0,                  0 }
    };

    const struct data_type *ptr;
    char *end;

    for (ptr = data_types; ptr->tag; ptr++)
    {
        if (strncmp( ptr->tag, buffer, ptr->len )) continue;
        *parse_type = ptr->parse_type;
        if ((*type = ptr->type) != -1) return ptr->len;
        /* "hex(xx):" is special */
        *type = (int)strtoul( buffer + 4, &end, 16 );
        if ((end <= buffer) || strncmp( end, "):", 2 )) return 0;
        return end + 2 - buffer;
    }
    return 0;
}

/* load and create a key from the input file */
static struct key *load_key( struct key *base, const char *buffer, int prefix_len,
                             struct file_load_info *info, timeout_t *modif )
{
    WCHAR *p;
    struct unicode_str name;
    int res;
    unsigned int mod;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;

    len = info->tmplen;
    if ((res = parse_strW( info->tmp, &len, buffer, ']' )) == -1)
    {
        file_read_error( "Malformed key", info );
        return NULL;
    }
    if (sscanf( buffer + res, " %u", &mod ) == 1)
        *modif = (timeout_t)mod * TICKS_PER_SEC + ticks_1601_to_1970;
    else
        *modif = current_time;

    p = info->tmp;
    while (prefix_len && *p) { if (*p++ == '\\') prefix_len--; }

    if (!*p)
    {
        if (prefix_len > 1)
        {
            file_read_error( "Malformed key", info );
            return NULL;
        }
        /* empty key name, return base key */
        return (struct key *)grab_object( base );
    }
    name.str = p;
    name.len = len - (p - info->tmp + 1) * sizeof(WCHAR);
    return create_key_recursive( base, &name, 0 );
}

/* update the modification time of a key (and its parents) after it has been loaded from a file */
static void update_key_time( struct key *key, timeout_t modif )
{
    while (key && !key->modif)
    {
        key->modif = modif;
        key = key->parent;
    }
}

/* load a global option from the input file */
static int load_global_option( const char *buffer, struct file_load_info *info )
{
    const char *p;

    if (!strncmp( buffer, "#arch=", 6 ))
    {
        enum prefix_type type;
        p = buffer + 6;
        if (!strcmp( p, "win32" )) type = PREFIX_32BIT;
        else if (!strcmp( p, "win64" )) type = PREFIX_64BIT;
        else
        {
            file_read_error( "Unknown architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
        if (prefix_type == PREFIX_UNKNOWN) prefix_type = type;
        else if (type != prefix_type)
        {
            file_read_error( "Mismatched architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
    }
    /* ignore unknown options */
    return 1;
}

/* load a key option from the input file */
static int load_key_option( struct key *key, const char *buffer, struct file_load_info *info )
{
    const char *p;
    data_size_t len;

    if (!strncmp( buffer, "#time=", 6 ))
    {
        timeout_t modif = 0;
        for (p = buffer + 6; *p; p++)
        {
            if (*p >= '0' && *p <= '9') modif = (modif << 4) | (*p - '0');
            else if (*p >= 'A' && *p <= 'F') modif = (modif << 4) | (*p - 'A' + 10);
            else if (*p >= 'a' && *p <= 'f') modif = (modif << 4) | (*p - 'a' + 10);
            else break;
        }
        update_key_time( key, modif );
    }
    if (!strncmp( buffer, "#class=", 7 ))
    {
        p = buffer + 7;
        if (*p++ != '"') return 0;
        if (!get_file_tmp_space( info, strlen(p) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if (parse_strW( info->tmp, &len, p, '\"' ) == -1) return 0;
        free( key->class );
        if (!(key->class = memdup( info->tmp, len ))) len = 0;
        key->classlen = len;
    }
    if (!strncmp( buffer, "#link", 5 )) key->flags |= KEY_SYMLINK;
    /* ignore unknown options */
    return 1;
}

/* parse a comma-separated list of hex digits */
static int parse_hex( unsigned char *dest, data_size_t *len, const char *buffer )
{
    const char *p = buffer;
    data_size_t count = 0;
    char *end;

    while (isxdigit(*p))
    {
        unsigned int val = strtoul( p, &end, 16 );
        if (end == p || val > 0xff) return -1;
        if (count++ >= *len) return -1;  /* dest buffer overflow */
        *dest++ = val;
        p = end;
        while (isspace(*p)) p++;
        if (*p == ',') p++;
        while (isspace(*p)) p++;
    }
    *len = count;
    return p - buffer;
}

/* parse a value name and create the corresponding value */
static struct key_value *parse_value_name( struct key *key, const char *buffer, data_size_t *len,
                                           struct file_load_info *info )
{
    struct key_value *value;
    struct unicode_str name;
    int index;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;
    name.str = info->tmp;
    name.len = info->tmplen;
    if (buffer[0] == '@')
    {
        name.len = 0;
        *len = 1;
    }
    else
    {
        int r = parse_strW( info->tmp, &name.len, buffer + 1, '\"' );
        if (r == -1) goto error;
        *len = r + 1; /* for initial quote */
        name.len -= sizeof(WCHAR);  /* terminating null */
    }
    while (isspace(buffer[*len])) (*len)++;
    if (buffer[*len] != '=') goto error;
    (*len)++;
    while (isspace(buffer[*len])) (*len)++;
    if (!(value = find_value( key, &name, &index ))) value = insert_value( key, &name, index );
    return value;

 error:
    file_read_error( "Malformed value name", info );
    return NULL;
}

/* load a value from the input file */
static int load_value( struct key *key, const char *buffer, struct file_load_info *info )
{
    DWORD dw;
    void *ptr, *newptr;
    int res, type, parse_type;
    data_size_t maxlen, len;
    struct key_value *value;

    if (!(value = parse_value_name( key, buffer, &len, info ))) return 0;
    if (!(res = get_data_type( buffer + len, &type, &parse_type ))) goto error;
    buffer += len + res;

    switch(parse_type)
    {
    case REG_SZ:
        if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if ((res = parse_strW( info->tmp, &len, buffer, '\"' )) == -1) goto error;
        ptr = info->tmp;
        break;
    case REG_DWORD:
        dw = strtoul( buffer, NULL, 16 );
        ptr = &dw;
        len = sizeof(dw);
        break;
    case REG_BINARY:  /* hex digits */
        len = 0;
        for (;;)
        {
            maxlen = 1 + strlen(buffer) / 2;  /* at least 2 chars for one hex byte */
            if (!get_file_tmp_space( info, len + maxlen )) return 0;
            if ((res = parse_hex( (unsigned char *)info->tmp + len, &maxlen, buffer )) == -1) goto error;
            len += maxlen;
            buffer += res;
            while (isspace(*buffer)) buffer++;
            if (!*buffer) break;
            if (*buffer != '\\') goto error;
            if (read_next_line( info) != 1) goto error;
            buffer = info->buffer;
            while (isspace(*buffer)) buffer++;
        }
        ptr = info->tmp;
        break;
    default:
        assert(0);
        ptr = NULL;  /* keep compiler quiet */
        break;
    }

    if (!len) newptr = NULL;
    else if (!(newptr = memdup( ptr, len ))) return 0;

    free( value->data );
    value->data = newptr;
    value->len  = len;
    value->type = type;
    return 1;

 error:
    file_read_error( "Malformed value", info );
    free( value->data );
    value->data = NULL;
    value->len  = 0;
    value->type = REG_NONE;
    return 0;
}

/* return the length (in path elements) of name that is part of the key name */
/* for instance if key is USER\foo\bar and name is foo\bar\baz, return 2 */
static int get_prefix_len( struct key *key, const char *name, struct file_load_info *info )
{
    WCHAR *p;
    int res;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(name) * sizeof(WCHAR) )) return 0;

    len = info->tmplen;
    if ((res = parse_strW( info->tmp, &len, name, ']' )) == -1)
    {
        file_read_error( "Malformed key", info );
        return 0;
    }
    for (p = info->tmp; *p; p++) if (*p == '\\') break;
    len = (p - info->tmp) * sizeof(WCHAR);
    for (res = 1; key != root_key; res++)
    {
        if (len == key->obj.name->len && !memicmpW( info->tmp, key->obj.name->name, len / sizeof(WCHAR) )) break;
        key = key->parent;
    }
    if (key == root_key) res = 0;  /* no matching name */
    return res;
}

/* load all the keys from the input file */
/* prefix_len is the number of key name prefixes to skip, or -1 for autodetection */
static void load_keys( struct key *key, const char *filename, FILE *f, int prefix_len )
{
    struct key *subkey = NULL;
    struct file_load_info info;
    timeout_t modif = current_time;
    char *p;

    info.filename = filename;
    info.file   = f;
    info.len    = 4;
    info.tmplen = 4;
    info.line   = 0;
    if (!(info.buffer = mem_alloc( info.len ))) return;
    if (!(info.tmp = mem_alloc( info.tmplen )))
    {
        free( info.buffer );
        return;
    }

    if ((read_next_line( &info ) != 1) ||
        strcmp( info.buffer, "WINE REGISTRY Version 2" ))
    {
        set_error( STATUS_NOT_REGISTRY_FILE );
        goto done;
    }

    while (read_next_line( &info ) == 1)
    {
        p = info.buffer;
        while (*p && isspace(*p)) p++;
        switch(*p)
        {
        case '[':   /* new key */
            if (subkey)
            {
                update_key_time( subkey, modif );
                release_object( subkey );
            }
            if (prefix_len == -1) prefix_len = get_prefix_len( key, p + 1, &info );
            if (!(subkey = load_key( key, p + 1, prefix_len, &info, &modif )))
                file_read_error( "Error creating key", &info );
            break;
        case '@':   /* default value */
        case '\"':  /* value */
            if (subkey) load_value( subkey, p, &info );
            else file_read_error( "Value without key", &info );
            break;
        case '#':   /* option */
            if (subkey) load_key_option( subkey, p, &info );
            else if (!load_global_option( p, &info )) goto done;
            break;
        case ';':   /* comment */
        case 0:     /* empty line */
            break;
        default:
            file_read_error( "Unrecognized input", &info );
            break;
        }
    }

 done:
    if (subkey)
    {
        update_key_time( subkey, modif );
        release_object( subkey );
    }
    free( info.buffer );
    free( info.tmp );
}

/* load a part of the registry from a file */
static void load_registry( struct key *key, obj_handle_t handle )
{
    struct file *file;
    int fd;

    if (!(file = get_file_obj( current->process, handle, FILE_READ_DATA ))) return;
    fd = dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = fdopen( fd, "r" );
        if (f)
        {
            load_keys( key, NULL, f, -1 );
            fclose( f );
        }
        else file_set_error();
    }
}

/* load one of the initial registry files */
static int load_init_registry_from_file( const char *filename, struct key *key )
{
    FILE *f;

    if ((f = fopen( filename, "r" )))
    {
        load_keys( key, filename, f, 0 );
        fclose( f );
        if (get_error() == STATUS_NOT_REGISTRY_FILE)
        {
            fprintf( stderr, "%s is not a valid registry file\n", filename );
            return 1;
        }
    }

    assert( save_branch_count < MAX_SAVE_BRANCH_INFO );

    save_branch_info[save_branch_count].path = filename;
    save_branch_info[save_branch_count++].key = (struct key *)grab_object( key );
    make_object_static( &key->obj );
    return (f != NULL);
}

static WCHAR *format_user_registry_path( const SID *sid, struct unicode_str *path )
{
    static const WCHAR prefixW[] = {'U','s','e','r','\\','S',0};
    static const WCHAR formatW[] = {'-','%','u',0};
    WCHAR buffer[7 + 10 + 10 + 10 * SID_MAX_SUB_AUTHORITIES];
    WCHAR *p = buffer;
    unsigned int i;

    strcpyW( p, prefixW );
    p += strlenW( prefixW );
    p += sprintfW( p, formatW, sid->Revision );
    p += sprintfW( p, formatW, MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5],
                                                   sid->IdentifierAuthority.Value[4] ),
                                         MAKEWORD( sid->IdentifierAuthority.Value[3],
                                                   sid->IdentifierAuthority.Value[2] )));
    for (i = 0; i < sid->SubAuthorityCount; i++)
        p += sprintfW( p, formatW, sid->SubAuthority[i] );

    path->len = (p - buffer) * sizeof(WCHAR);
    path->str = p = memdup( buffer, path->len );
    return p;
}

/* get the cpu architectures that can be supported in the current prefix */
unsigned int get_prefix_cpu_mask(void)
{
    /* Allowed server/client/prefix combinations:
     *
     *              prefix
     *            32     64
     *  server +------+------+ client
     *         |  ok  | fail | 32
     *      32 +------+------+---
     *         | fail | fail | 64
     *      ---+------+------+---
     *         |  ok  |  ok  | 32
     *      64 +------+------+---
     *         | fail |  ok  | 64
     *      ---+------+------+---
     */
    switch (prefix_type)
    {
    case PREFIX_64BIT:
        /* 64-bit prefix requires 64-bit server */
        return sizeof(void *) > sizeof(int) ? ~0 : 0;
    case PREFIX_32BIT:
    default:
        return ~CPU_64BIT_MASK;  /* only 32-bit cpus supported on 32-bit prefix */
    }
}

void create_wow_key(struct key *parent, const struct unicode_str *name, unsigned int flags)
{
    struct key *key = create_key_recursive( parent, name, current_time );
    key->flags |= flags;
    release_object( key );
}

/* registry initialisation */
void init_registry(void)
{
    static const WCHAR HKLM[] = { 'M','a','c','h','i','n','e' };
    static const WCHAR HKU_default[] = { 'U','s','e','r','\\','.','D','e','f','a','u','l','t' };
    static const struct unicode_str HKLM_name = { HKLM, sizeof(HKLM) };
    static const struct unicode_str HKU_name = { HKU_default, sizeof(HKU_default) };

    WCHAR *current_user_path;
    struct unicode_str current_user_str;
    struct key *key, *hklm, *hkcu;
    char *p;

    /* switch to the config dir */

    if (fchdir( config_dir_fd ) == -1) fatal_error( "chdir to config dir: %s\n", strerror( errno ));

    /* create the root key */
    root_key = create_key( NULL, &root_name_str, NULL, 0, 0, 0, NULL );
    assert( root_key );
    make_object_static( &root_key->obj );

    /* load system.reg into Registry\Machine */

    if (!(hklm = create_key_recursive( root_key, &HKLM_name, current_time )))
        fatal_error( "could not create Machine registry key\n" );

    if (!load_init_registry_from_file( "system.reg", hklm ))
    {
        if ((p = getenv( "WINEARCH" )) && !strcmp( p, "win32" ))
            prefix_type = PREFIX_32BIT;
        else
            prefix_type = sizeof(void *) > sizeof(int) ? PREFIX_64BIT : PREFIX_32BIT;
    }
    else if (prefix_type == PREFIX_UNKNOWN)
        prefix_type = PREFIX_32BIT;

    /* load userdef.reg into Registry\User\.Default */

    if (!(key = create_key_recursive( root_key, &HKU_name, current_time )))
        fatal_error( "could not create User\\.Default registry key\n" );

    load_init_registry_from_file( "userdef.reg", key );
    release_object( key );

    /* load user.reg into HKEY_CURRENT_USER */

    /* FIXME: match default user in token.c. should get from process token instead */
    current_user_path = format_user_registry_path( security_local_user_sid, &current_user_str );
    if (!current_user_path ||
        !(hkcu = create_key_recursive( root_key, &current_user_str, current_time )))
        fatal_error( "could not create HKEY_CURRENT_USER registry key\n" );
    free( current_user_path );
    load_init_registry_from_file( "user.reg", hkcu );

    if (prefix_type == PREFIX_64BIT)
    {
        static const WCHAR softwareW[] = {'S','o','f','t','w','a','r','e'};
        static const WCHAR classesW[] = {'C','l','a','s','s','e','s'};
        static const WCHAR clsidW[] = {'C','L','S','I','D'};
        static const WCHAR directshowW[] = {'D','i','r','e','c','t','S','h','o','w'};
        static const WCHAR interfaceW[] = {'I','n','t','e','r','f','a','c','e'};
        static const WCHAR media_typeW[] = {'M','e','d','i','a',' ','T','y','p','e'};
        static const WCHAR mediafoundationW[] = {'M','e','d','i','a','F','o','u','n','d','a','t','i','o','n'};
        static const struct unicode_str software_name = { softwareW, sizeof(softwareW) };
        static const struct unicode_str classes_name = { classesW, sizeof(classesW) };
        static const struct unicode_str clsid_name = { clsidW, sizeof(clsidW) };
        static const struct unicode_str directshow_name = { directshowW, sizeof(directshowW) };
        static const struct unicode_str interface_name = { interfaceW, sizeof(interfaceW) };
        static const struct unicode_str media_type_name = { media_typeW, sizeof(media_typeW) };
        static const struct unicode_str mediafoundation_name = { mediafoundationW, sizeof(mediafoundationW) };

        struct key *software = create_key_recursive( hklm, &software_name, current_time );
        struct key *classes = create_key_recursive( software, &classes_name, current_time );
        struct key *classes_wow64 = create_key_recursive( classes, &wow6432node_str, current_time );

        /* set the WOW64 flag on HKLM\Software */
        software->flags |= KEY_WOW64;
        create_wow_key( software, &wow6432node_str, 0 );

        /* set the shared flag on HKLM\Software\Classes */
        classes->flags |= KEY_WOWSHARE;

        /* set the WOW64 flags on Classes subkeys */
        create_wow_key( classes, &clsid_name, KEY_WOW64 );
        create_wow_key( classes_wow64, &clsid_name, 0 );
        create_wow_key( classes, &directshow_name, KEY_WOW64 );
        create_wow_key( classes_wow64, &directshow_name, 0 );
        create_wow_key( classes, &interface_name, KEY_WOW64 );
        create_wow_key( classes_wow64, &interface_name, 0 );
        create_wow_key( classes, &media_type_name, KEY_WOW64 );
        create_wow_key( classes_wow64, &media_type_name, 0 );
        create_wow_key( classes, &mediafoundation_name, KEY_WOW64 );
        create_wow_key( classes_wow64, &mediafoundation_name, 0 );

        release_object( classes_wow64 );
        release_object( classes );
        release_object( software );

        /* FIXME: handle HKCU too */
    }

    release_object( hklm );
    release_object( hkcu );
    release_object( root_key );

    /* start the periodic save timer */
    set_periodic_save_timer();

    /* go back to the server dir */
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
}

/* save a registry branch to a file */
static void save_all_subkeys( struct key *key, FILE *f )
{
    fprintf( f, "WINE REGISTRY Version 2\n" );
    fprintf( f, ";; All keys relative to " );
    dump_path( key, NULL, f );
    fprintf( f, "\n" );
    switch (prefix_type)
    {
    case PREFIX_32BIT:
        fprintf( f, "\n#arch=win32\n" );
        break;
    case PREFIX_64BIT:
        fprintf( f, "\n#arch=win64\n" );
        break;
    default:
        break;
    }
    save_subkeys( key, key, f );
}

/* save a registry branch to a file handle */
static void save_registry( struct key *key, obj_handle_t handle )
{
    struct file *file;
    int fd;

    if (!(file = get_file_obj( current->process, handle, FILE_WRITE_DATA ))) return;
    fd = dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = fdopen( fd, "w" );
        if (f)
        {
            save_all_subkeys( key, f );
            if (fclose( f )) file_set_error();
        }
        else
        {
            file_set_error();
            close( fd );
        }
    }
}

/* save a registry branch to a file */
static int save_branch( struct key *key, const char *path )
{
    struct stat st;
    char *p, *tmp = NULL;
    int fd, count = 0, ret = 0;
    FILE *f;

    if (!(key->flags & KEY_DIRTY))
    {
        if (debug_level > 1) dump_operation( key, NULL, "Not saving clean" );
        return 1;
    }

    /* test the file type */

    if ((fd = open( path, O_WRONLY )) != -1)
    {
        /* if file is not a regular file or has multiple links or is accessed
         * via symbolic links, write directly into it; otherwise use a temp file */
        if (!lstat( path, &st ) && (!S_ISREG(st.st_mode) || st.st_nlink > 1))
        {
            ftruncate( fd, 0 );
            goto save;
        }
        close( fd );
    }

    /* create a temp file in the same directory */

    if (!(tmp = malloc( strlen(path) + 20 ))) goto done;
    strcpy( tmp, path );
    if ((p = strrchr( tmp, '/' ))) p++;
    else p = tmp;
    for (;;)
    {
        sprintf( p, "reg%lx%04x.tmp", (long) getpid(), count++ );
        if ((fd = open( tmp, O_CREAT | O_EXCL | O_WRONLY, 0666 )) != -1) break;
        if (errno != EEXIST) goto done;
        close( fd );
    }

    /* now save to it */

 save:
    if (!(f = fdopen( fd, "w" )))
    {
        if (tmp) unlink( tmp );
        close( fd );
        goto done;
    }

    if (debug_level > 1)
    {
        fprintf( stderr, "%s: ", path );
        dump_operation( key, NULL, "saving" );
    }

    save_all_subkeys( key, f );
    ret = !fclose(f);

    if (tmp)
    {
        /* if successfully written, rename to final name */
        if (ret) ret = !rename( tmp, path );
        if (!ret) unlink( tmp );
    }

done:
    free( tmp );
    if (ret) make_clean( key );
    return ret;
}

/* periodic saving of the registry */
static void periodic_save( void *arg )
{
    int i;

    if (fchdir( config_dir_fd ) == -1) return;
    save_timeout_user = NULL;
    for (i = 0; i < save_branch_count; i++)
        save_branch( save_branch_info[i].key, save_branch_info[i].path );
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
    set_periodic_save_timer();
}

/* start the periodic save timer */
static void set_periodic_save_timer(void)
{
    if (save_timeout_user) remove_timeout_user( save_timeout_user );
    save_timeout_user = add_timeout_user( save_period, periodic_save, NULL );
}

/* save the modified registry branches to disk */
void flush_registry(void)
{
    int i;

    if (fchdir( config_dir_fd ) == -1) return;
    for (i = 0; i < save_branch_count; i++)
    {
        if (!save_branch( save_branch_info[i].key, save_branch_info[i].path ))
        {
            fprintf( stderr, "wineserver: could not save registry branch to %s",
                     save_branch_info[i].path );
            perror( " " );
        }
    }
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));

    delete_key( root_key, 1 );
}

/* determine if the thread is wow64 (32-bit client running on 64-bit prefix) */
static int is_wow64_thread( struct thread *thread )
{
    return (prefix_type == PREFIX_64BIT && !(CPU_FLAG(thread->process->cpu) & CPU_64BIT_MASK));
}


/* create a registry key */
DECL_HANDLER(create_key)
{
    struct key *key = NULL, *parent = NULL;
    struct unicode_str name = get_req_unicode_str();
    struct unicode_str class;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, NULL );
    unsigned int attributes;

    if (!objattr) return;

    attributes = objattr->attributes;

    class.str = get_req_data_after_objattr( objattr, &class.len );
    class.len = (class.len / sizeof(WCHAR)) * sizeof(WCHAR);

    /* NOTE: no access rights are required from the parent handle to create a key */
    if (objattr->rootdir && !(parent = get_hkey_obj( objattr->rootdir, 0 )))
        return;

    if (is_wow64_thread( current ) && !(req->access & KEY_WOW64_64KEY))
        attributes |= WINE_OBJ_WOW64;

    if (parent && (req->access & KEY_WOW64_32KEY))
    {
        struct unicode_str token = {0};
        get_path_token( &name, &token );
        parent = find_wow64_subkey( parent, &token );
    }

    if ((key = create_key( parent ? &parent->obj : NULL, &name, &class, req->options,
                           req->access, attributes, sd )))
    {
        if (get_error() == STATUS_SUCCESS)
            reply->created = 1;
        else if (get_error() == STATUS_OBJECT_NAME_EXISTS)
        {
            reply->created = 0;
            clear_error();
        }

        reply->hkey = alloc_handle( current->process, key, req->access, objattr->attributes );
        release_object( key );
    }

    if (parent) release_object( parent );
}

/* open a registry key */
DECL_HANDLER(open_key)
{
    struct key *key, *parent = NULL;
    struct unicode_str name = get_req_unicode_str();
    unsigned int attributes = req->attributes;

    if (name.len >= 65534)
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return;
    }

    /* NOTE: no access rights are required to open the parent key, only the child key */
    if (req->parent && !(parent = get_hkey_obj( req->parent, 0 )))
        return;

    if (is_wow64_thread( current ) && !(req->access & KEY_WOW64_64KEY))
        attributes |= WINE_OBJ_WOW64;

    if (parent && (req->access & KEY_WOW64_32KEY))
    {
        struct unicode_str token = {0};
        get_path_token( &name, &token );
        parent = find_wow64_subkey( parent, &token );
    }

    if ((key = (struct key *)open_named_object( parent ? &parent->obj : NULL,
                                                &key_ops, &name, attributes )))
    {
        reply->hkey = alloc_handle( current->process, key, req->access, req->attributes );
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* delete a registry key */
DECL_HANDLER(delete_key)
{
    struct key *key;

    if ((key = get_hkey_obj( req->hkey, DELETE )))
    {
        delete_key( key, 0);
        release_object( key );
    }
}

/* flush a registry key */
DECL_HANDLER(flush_key)
{
    struct key *key = get_hkey_obj( req->hkey, 0 );
    if (key)
    {
        /* we don't need to do anything here with the current implementation */
        release_object( key );
    }
}

/* enumerate registry subkeys */
DECL_HANDLER(enum_key)
{
    struct key *key;

    if ((key = get_hkey_obj( req->hkey,
                             req->index == -1 ? KEY_QUERY_VALUE : KEY_ENUMERATE_SUB_KEYS )))
    {
        enum_key( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* set a value of a registry key */
DECL_HANDLER(set_key_value)
{
    struct key *key;
    struct unicode_str name;

    if (req->namelen > get_req_data_size())
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    name.str = get_req_data();
    name.len = (req->namelen / sizeof(WCHAR)) * sizeof(WCHAR);

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        data_size_t datalen = get_req_data_size() - req->namelen;
        const char *data = (const char *)get_req_data() + req->namelen;

        set_value( key, &name, req->type, data, datalen );
        release_object( key );
    }
}

/* retrieve the value of a registry key */
DECL_HANDLER(get_key_value)
{
    struct key *key;
    struct unicode_str name = get_req_unicode_str();

    reply->total = 0;
    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        get_value( key, &name, &reply->type, &reply->total );
        release_object( key );
    }
}

/* enumerate the value of a registry key */
DECL_HANDLER(enum_key_value)
{
    struct key *key;

    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        enum_value( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* delete a value of a registry key */
DECL_HANDLER(delete_key_value)
{
    struct key *key;
    struct unicode_str name = get_req_unicode_str();

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        delete_value( key, &name );
        release_object( key );
    }
}

/* load a registry branch from a file */
DECL_HANDLER(load_registry)
{
    struct key *key;
    struct object *parent = NULL;
    struct unicode_str name;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, NULL );

    if (!objattr) return;

    if (!thread_single_check_privilege( current, &SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if (objattr->rootdir && !(parent = get_handle_obj( current->process, objattr->rootdir, 0, &key_ops )))
        return;

    if ((key = create_key( parent, &name, NULL, 0, KEY_WOW64_64KEY, 0, sd )))
    {
        load_registry( key, req->file );
        release_object( key );
    }

    if (parent) release_object( parent );
}

DECL_HANDLER(unload_registry)
{
    struct key *key;

    if (!thread_single_check_privilege( current, &SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if ((key = get_hkey_obj( req->hkey, 0 )))
    {
        delete_key( key, 1 );     /* FIXME */
        release_object( key );
    }
}

/* save a registry branch to a file */
DECL_HANDLER(save_registry)
{
    struct key *key;

    if (!thread_single_check_privilege( current, &SeBackupPrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if ((key = get_hkey_obj( req->hkey, 0 )))
    {
        save_registry( key, req->file );
        release_object( key );
    }
}

/* add a registry key change notification */
DECL_HANDLER(set_registry_notification)
{
    struct key *key;
    struct event *event;
    struct notify *notify;

    key = get_hkey_obj( req->hkey, KEY_NOTIFY );
    if (key)
    {
        event = get_event_obj( current->process, req->event, SYNCHRONIZE );
        if (event)
        {
            notify = find_notify( key, current->process, req->hkey );
            if (notify)
            {
                if (notify->event)
                    release_object( notify->event );
                grab_object( event );
                notify->event = event;
            }
            else
            {
                notify = mem_alloc( sizeof(*notify) );
                if (notify)
                {
                    grab_object( event );
                    notify->event   = event;
                    notify->subtree = req->subtree;
                    notify->filter  = req->filter;
                    notify->hkey    = req->hkey;
                    notify->process = current->process;
                    list_add_head( &key->notify_list, &notify->entry );
                }
            }
            if (notify)
            {
                reset_event( event );
                set_error( STATUS_PENDING );
            }
            release_object( event );
        }
        release_object( key );
    }
}
