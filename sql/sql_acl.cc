/* Copyright (C) 2000-2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/*
  The privileges are saved in the following tables:
  mysql/user	 ; super user who are allowed to do almost anything
  mysql/host	 ; host privileges. This is used if host is empty in mysql/db.
  mysql/db	 ; database privileges / user

  data in tables is sorted according to how many not-wild-cards there is
  in the relevant fields. Empty strings comes last.
*/

#include "mysql_priv.h"
#include "sql_acl.h"
#include "hash_filo.h"
#ifdef HAVE_REPLICATION
#include "sql_repl.h" //for tables_ok()
#endif
#include <m_ctype.h>
#include <stdarg.h>

#ifndef NO_EMBEDDED_ACCESS_CHECKS

class acl_entry :public hash_filo_element
{
public:
  ulong access;
  uint16 length;
  char key[1];					// Key will be stored here
};


static byte* acl_entry_get_key(acl_entry *entry,uint *length,
			       my_bool not_used __attribute__((unused)))
{
  *length=(uint) entry->length;
  return (byte*) entry->key;
}

#define IP_ADDR_STRLEN (3+1+3+1+3+1+3)
#define ACL_KEY_LENGTH (IP_ADDR_STRLEN+1+NAME_LEN+1+USERNAME_LENGTH+1)

static DYNAMIC_ARRAY acl_hosts,acl_users,acl_dbs;
static MEM_ROOT mem, memex;
static bool initialized=0;
static bool allow_all_hosts=1;
static HASH acl_check_hosts, column_priv_hash;
static DYNAMIC_ARRAY acl_wild_hosts;
static hash_filo *acl_cache;
static uint grant_version=0;
static uint priv_version=0; /* Version of priv tables. incremented by acl_init */
static ulong get_access(TABLE *form,uint fieldnr, uint *next_field=0);
static int acl_compare(ACL_ACCESS *a,ACL_ACCESS *b);
static ulong get_sort(uint count,...);
static void init_check_host(void);
static ACL_USER *find_acl_user(const char *host, const char *user);
static bool update_user_table(THD *thd, const char *host, const char *user,
			      const char *new_password, uint new_password_len);
static void update_hostname(acl_host_and_ip *host, const char *hostname);
static bool compare_hostname(const acl_host_and_ip *host,const char *hostname,
			     const char *ip);

/*
  Convert scrambled password to binary form, according to scramble type, 
  Binary form is stored in user.salt.
*/

static
void
set_user_salt(ACL_USER *acl_user, const char *password, uint password_len)
{
  if (password_len == SCRAMBLED_PASSWORD_CHAR_LENGTH)
  {
    get_salt_from_password(acl_user->salt, password);
    acl_user->salt_len= SCRAMBLE_LENGTH;
  }
  else if (password_len == SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
  {
    get_salt_from_password_323((ulong *) acl_user->salt, password);
    acl_user->salt_len= SCRAMBLE_LENGTH_323;
  }
  else
    acl_user->salt_len= 0;
}

/*
  This after_update function is used when user.password is less than
  SCRAMBLE_LENGTH bytes.
*/

static void restrict_update_of_old_passwords_var(THD *thd,
                                                 enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL)
  {
    pthread_mutex_lock(&LOCK_global_system_variables);
    global_system_variables.old_passwords= 1;
    pthread_mutex_unlock(&LOCK_global_system_variables);
  }
  else
    thd->variables.old_passwords= 1;
}


/*
  Read grant privileges from the privilege tables in the 'mysql' database.

  SYNOPSIS
    acl_init()
    thd				Thread handler
    dont_read_acl_tables	Set to 1 if run with --skip-grant

  RETURN VALUES
    0	ok
    1	Could not initialize grant's
*/


my_bool acl_init(THD *org_thd, bool dont_read_acl_tables)
{
  THD  *thd;
  TABLE_LIST tables[3];
  TABLE *table;
  READ_RECORD read_record_info;
  MYSQL_LOCK *lock;
  my_bool return_val=1;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  DBUG_ENTER("acl_init");

  if (!acl_cache)
    acl_cache=new hash_filo(ACL_CACHE_SIZE,0,0,
			    (hash_get_key) acl_entry_get_key,
			    (hash_free_key) free, system_charset_info);
  if (dont_read_acl_tables)
  {
    DBUG_RETURN(0); /* purecov: tested */
  }

  priv_version++; /* Privileges updated */
  mysql_proc_table_exists= 1;			// Assume mysql.proc exists

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD))
    DBUG_RETURN(1); /* purecov: inspected */
  thd->store_globals();

  acl_cache->clear(1);				// Clear locked hostname cache
  thd->db= my_strdup("mysql",MYF(0));
  thd->db_length=5;				// Safety
  bzero((char*) &tables,sizeof(tables));
  tables[0].alias=tables[0].real_name=(char*) "host";
  tables[1].alias=tables[1].real_name=(char*) "user";
  tables[2].alias=tables[2].real_name=(char*) "db";
  tables[0].next_local= tables[0].next_global= tables+1;
  tables[1].next_local= tables[1].next_global= tables+2;
  tables[0].lock_type=tables[1].lock_type=tables[2].lock_type=TL_READ;
  tables[0].db=tables[1].db=tables[2].db=thd->db;

  uint counter;
  if (open_tables(thd, tables, &counter))
  {
    sql_print_error("Fatal error: Can't open privilege tables: %s",
		    thd->net.last_error);
    goto end;
  }
  TABLE *ptr[3];				// Lock tables for quick update
  ptr[0]= tables[0].table;
  ptr[1]= tables[1].table;
  ptr[2]= tables[2].table;
  if (!(lock=mysql_lock_tables(thd,ptr,3)))
  {
    sql_print_error("Fatal error: Can't lock privilege tables: %s",
		    thd->net.last_error);
    goto end;
  }
  init_sql_alloc(&mem, ACL_ALLOC_BLOCK_SIZE, 0);
  init_read_record(&read_record_info,thd,table= tables[0].table,NULL,1,0);
  VOID(my_init_dynamic_array(&acl_hosts,sizeof(ACL_HOST),20,50));
  while (!(read_record_info.read_record(&read_record_info)))
  {
    ACL_HOST host;
    update_hostname(&host.host,get_field(&mem, table->field[0]));
    host.db=	 get_field(&mem, table->field[1]);
    host.access= get_access(table,2);
    host.access= fix_rights_for_db(host.access);
    host.sort=	 get_sort(2,host.host.hostname,host.db);
    if (check_no_resolve && hostname_requires_resolving(host.host.hostname))
    {
      sql_print_warning("'host' entry '%s|%s' "
		      "ignored in --skip-name-resolve mode.",
		      host.host.hostname, host.db, host.host.hostname);
      continue;
    }
#ifndef TO_BE_REMOVED
    if (table->fields ==  8)
    {						// Without grant
      if (host.access & CREATE_ACL)
	host.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL | CREATE_TMP_ACL;
    }
#endif
    VOID(push_dynamic(&acl_hosts,(gptr) &host));
  }
  qsort((gptr) dynamic_element(&acl_hosts,0,ACL_HOST*),acl_hosts.elements,
	sizeof(ACL_HOST),(qsort_cmp) acl_compare);
  end_read_record(&read_record_info);
  freeze_size(&acl_hosts);

  init_read_record(&read_record_info,thd,table=tables[1].table,NULL,1,0);
  VOID(my_init_dynamic_array(&acl_users,sizeof(ACL_USER),50,100));
  if (table->field[2]->field_length < SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
  {
    sql_print_error("Fatal error: mysql.user table is damaged or in "
                    "unsupported 3.20 format.");
    goto end;
  }

  DBUG_PRINT("info",("user table fields: %d, password length: %d",
		     table->fields, table->field[2]->field_length));
  
  pthread_mutex_lock(&LOCK_global_system_variables);
  if (table->field[2]->field_length < SCRAMBLED_PASSWORD_CHAR_LENGTH)
  {
    if (opt_secure_auth)
    {
      pthread_mutex_unlock(&LOCK_global_system_variables);
      sql_print_error("Fatal error: mysql.user table is in old format, "
                      "but server started with --secure-auth option.");
      goto end;
    }
    sys_old_passwords.after_update= restrict_update_of_old_passwords_var;
    if (global_system_variables.old_passwords)
      pthread_mutex_unlock(&LOCK_global_system_variables);
    else
    {
      global_system_variables.old_passwords= 1;
      pthread_mutex_unlock(&LOCK_global_system_variables);
      sql_print_warning("mysql.user table is not updated to new password format; "
                        "Disabling new password usage until "
                        "mysql_fix_privilege_tables is run");
    }
    thd->variables.old_passwords= 1;
  }
  else
  {
    sys_old_passwords.after_update= 0;
    pthread_mutex_unlock(&LOCK_global_system_variables);
  }

  allow_all_hosts=0;
  while (!(read_record_info.read_record(&read_record_info)))
  {
    ACL_USER user;
    update_hostname(&user.host, get_field(&mem, table->field[0]));
    user.user= get_field(&mem, table->field[1]);
    if (check_no_resolve && hostname_requires_resolving(user.host.hostname))
    {
      sql_print_warning("'user' entry '%s@%s' "
                        "ignored in --skip-name-resolve mode.",
		      user.user, user.host.hostname, user.host.hostname);
      continue;
    }

    const char *password= get_field(&mem, table->field[2]);
    uint password_len= password ? strlen(password) : 0;
    set_user_salt(&user, password, password_len);
    if (user.salt_len == 0 && password_len != 0)
    {
      switch (password_len) {
      case 45: /* 4.1: to be removed */
        sql_print_warning("Found 4.1 style password for user '%s@%s'. "
                          "Ignoring user. "
                          "You should change password for this user.",
                          user.user ? user.user : "",
                          user.host.hostname ? user.host.hostname : "");
        break;
      default:
        sql_print_warning("Found invalid password for user: '%s@%s'; "
                          "Ignoring user", user.user ? user.user : "",
                           user.host.hostname ? user.host.hostname : "");
        break;
      }
    }
    else                                        // password is correct
    {
      uint next_field;
      user.access= get_access(table,3,&next_field) & GLOBAL_ACLS;
      /*
        if it is pre 5.0.1 privilege table then map CREATE privilege on
        CREATE VIEW & SHOW VIEW privileges
      */
      if (table->fields <= 31 && (user.access & CREATE_ACL))
        user.access|= (CREATE_VIEW_ACL | SHOW_VIEW_ACL);
      user.sort= get_sort(2,user.host.hostname,user.user);
      user.hostname_length= (user.host.hostname ?
                             (uint) strlen(user.host.hostname) : 0);

      if (table->fields >= 31)	 /* Starting from 4.0.2 we have more fields */
      {
        char *ssl_type=get_field(&mem, table->field[next_field++]);
        if (!ssl_type)
          user.ssl_type=SSL_TYPE_NONE;
        else if (!strcmp(ssl_type, "ANY"))
          user.ssl_type=SSL_TYPE_ANY;
        else if (!strcmp(ssl_type, "X509"))
          user.ssl_type=SSL_TYPE_X509;
        else  /* !strcmp(ssl_type, "SPECIFIED") */
          user.ssl_type=SSL_TYPE_SPECIFIED;

        user.ssl_cipher=   get_field(&mem, table->field[next_field++]);
        user.x509_issuer=  get_field(&mem, table->field[next_field++]);
        user.x509_subject= get_field(&mem, table->field[next_field++]);

        char *ptr = get_field(&mem, table->field[next_field++]);
        user.user_resource.questions=ptr ? atoi(ptr) : 0;
        ptr = get_field(&mem, table->field[next_field++]);
        user.user_resource.updates=ptr ? atoi(ptr) : 0;
        ptr = get_field(&mem, table->field[next_field++]);
        user.user_resource.connections=ptr ? atoi(ptr) : 0;
        if (user.user_resource.questions || user.user_resource.updates ||
            user.user_resource.connections)
          mqh_used=1;
      }
      else
      {
        user.ssl_type=SSL_TYPE_NONE;
        bzero((char *)&(user.user_resource),sizeof(user.user_resource));
#ifndef TO_BE_REMOVED
        if (table->fields <= 13)
        {						// Without grant
          if (user.access & CREATE_ACL)
            user.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
        }
        /* Convert old privileges */
        user.access|= LOCK_TABLES_ACL | CREATE_TMP_ACL | SHOW_DB_ACL;
        if (user.access & FILE_ACL)
          user.access|= REPL_CLIENT_ACL | REPL_SLAVE_ACL;
        if (user.access & PROCESS_ACL)
          user.access|= SUPER_ACL | EXECUTE_ACL;
#endif
      }
      VOID(push_dynamic(&acl_users,(gptr) &user));
      if (!user.host.hostname || user.host.hostname[0] == wild_many &&
          !user.host.hostname[1])
        allow_all_hosts=1;			// Anyone can connect
    }
  }
  qsort((gptr) dynamic_element(&acl_users,0,ACL_USER*),acl_users.elements,
	sizeof(ACL_USER),(qsort_cmp) acl_compare);
  end_read_record(&read_record_info);
  freeze_size(&acl_users);

  init_read_record(&read_record_info,thd,table=tables[2].table,NULL,1,0);
  VOID(my_init_dynamic_array(&acl_dbs,sizeof(ACL_DB),50,100));
  while (!(read_record_info.read_record(&read_record_info)))
  {
    ACL_DB db;
    update_hostname(&db.host,get_field(&mem, table->field[0]));
    db.db=get_field(&mem, table->field[1]);
    if (!db.db)
    {
      sql_print_warning("Found an entry in the 'db' table with empty database name; Skipped");
      continue;
    }
    db.user=get_field(&mem, table->field[2]);
    if (check_no_resolve && hostname_requires_resolving(db.host.hostname))
    {
      sql_print_warning("'db' entry '%s %s@%s' "
		        "ignored in --skip-name-resolve mode.",
		        db.db, db.user, db.host.hostname, db.host.hostname);
      continue;
    }
    db.access=get_access(table,3);
    db.access=fix_rights_for_db(db.access);
    db.sort=get_sort(3,db.host.hostname,db.db,db.user);
#ifndef TO_BE_REMOVED
    if (table->fields <=  9)
    {						// Without grant
      if (db.access & CREATE_ACL)
	db.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
    }
#endif
    VOID(push_dynamic(&acl_dbs,(gptr) &db));
  }
  qsort((gptr) dynamic_element(&acl_dbs,0,ACL_DB*),acl_dbs.elements,
	sizeof(ACL_DB),(qsort_cmp) acl_compare);
  end_read_record(&read_record_info);
  freeze_size(&acl_dbs);
  init_check_host();

  mysql_unlock_tables(thd, lock);
  initialized=1;
  thd->version--;				// Force close to free memory
  return_val=0;

end:
  close_thread_tables(thd);
  delete thd;
  if (org_thd)
    org_thd->store_globals();			/* purecov: inspected */
  else
  {
    /* Remember that we don't have a THD */
    my_pthread_setspecific_ptr(THR_THD,  0);
  }
  DBUG_RETURN(return_val);
}


void acl_free(bool end)
{
  free_root(&mem,MYF(0));
  delete_dynamic(&acl_hosts);
  delete_dynamic(&acl_users);
  delete_dynamic(&acl_dbs);
  delete_dynamic(&acl_wild_hosts);
  hash_free(&acl_check_hosts);
  if (!end)
    acl_cache->clear(1); /* purecov: inspected */
  else
  {
    delete acl_cache;
    acl_cache=0;
  }
}


/*
  Forget current privileges and read new privileges from the privilege tables

  SYNOPSIS
    acl_reload()
    thd			Thread handle
*/

void acl_reload(THD *thd)
{
  DYNAMIC_ARRAY old_acl_hosts,old_acl_users,old_acl_dbs;
  MEM_ROOT old_mem;
  bool old_initialized;
  DBUG_ENTER("acl_reload");

  if (thd && thd->locked_tables)
  {					// Can't have locked tables here
    thd->lock=thd->locked_tables;
    thd->locked_tables=0;
    close_thread_tables(thd);
  }
  if ((old_initialized=initialized))
    VOID(pthread_mutex_lock(&acl_cache->lock));

  old_acl_hosts=acl_hosts;
  old_acl_users=acl_users;
  old_acl_dbs=acl_dbs;
  old_mem=mem;
  delete_dynamic(&acl_wild_hosts);
  hash_free(&acl_check_hosts);

  if (acl_init(thd, 0))
  {					// Error. Revert to old list
    DBUG_PRINT("error",("Reverting to old privileges"));
    acl_free();				/* purecov: inspected */
    acl_hosts=old_acl_hosts;
    acl_users=old_acl_users;
    acl_dbs=old_acl_dbs;
    mem=old_mem;
    init_check_host();
  }
  else
  {
    free_root(&old_mem,MYF(0));
    delete_dynamic(&old_acl_hosts);
    delete_dynamic(&old_acl_users);
    delete_dynamic(&old_acl_dbs);
  }
  if (old_initialized)
    VOID(pthread_mutex_unlock(&acl_cache->lock));
  DBUG_VOID_RETURN;
}


/*
  Get all access bits from table after fieldnr

  IMPLEMENTATION
  We know that the access privileges ends when there is no more fields
  or the field is not an enum with two elements.

  SYNOPSIS
    get_access()
    form        an open table to read privileges from.
                The record should be already read in table->record[0]
    fieldnr     number of the first privilege (that is ENUM('N','Y') field
    next_field  on return - number of the field next to the last ENUM
                (unless next_field == 0)

  RETURN VALUE
    privilege mask
*/

static ulong get_access(TABLE *form, uint fieldnr, uint *next_field)
{
  ulong access_bits=0,bit;
  char buff[2];
  String res(buff,sizeof(buff),&my_charset_latin1);
  Field **pos;

  for (pos=form->field+fieldnr, bit=1;
       *pos && (*pos)->real_type() == FIELD_TYPE_ENUM &&
	 ((Field_enum*) (*pos))->typelib->count == 2 ;
       pos++, fieldnr++, bit<<=1)
  {
    (*pos)->val_str(&res);
    if (my_toupper(&my_charset_latin1, res[0]) == 'Y')
      access_bits|= bit;
  }
  if (next_field)
    *next_field=fieldnr;
  return access_bits;
}


/*
  Return a number which, if sorted 'desc', puts strings in this order:
    no wildcards
    wildcards
    empty string
*/

static ulong get_sort(uint count,...)
{
  va_list args;
  va_start(args,count);
  ulong sort=0;

  /* Should not use this function with more than 4 arguments for compare. */
  DBUG_ASSERT(count <= 4);

  while (count--)
  {
    char *start, *str= va_arg(args,char*);
    uint chars= 0;
    uint wild_pos= 0;           /* first wildcard position */

    if ((start= str))
    {
      for (; *str ; str++)
      {
	if (*str == wild_many || *str == wild_one || *str == wild_prefix)
        {
          wild_pos= (uint) (str - start) + 1;
          break;
        }
        chars= 128;                             // Marker that chars existed
      }
    }
    sort= (sort << 8) + (wild_pos ? min(wild_pos, 127) : chars);
  }
  va_end(args);
  return sort;
}


static int acl_compare(ACL_ACCESS *a,ACL_ACCESS *b)
{
  if (a->sort > b->sort)
    return -1;
  if (a->sort < b->sort)
    return 1;
  return 0;
}


/*
  Seek ACL entry for a user, check password, SSL cypher, and if
  everything is OK, update THD user data and USER_RESOURCES struct.

  IMPLEMENTATION
   This function does not check if the user has any sensible privileges:
   only user's existence and  validity is checked.
   Note, that entire operation is protected by acl_cache_lock.

  SYNOPSIS
    acl_getroot()
    thd         thread handle. If all checks are OK,
                thd->priv_user, thd->master_access are updated.
                thd->host, thd->ip, thd->user are used for checks.
    mqh         user resources; on success mqh is reset, else
                unchanged
    passwd      scrambled & crypted password, received from client
                (to check): thd->scramble or thd->scramble_323 is
                used to decrypt passwd, so they must contain
                original random string,
    passwd_len  length of passwd, must be one of 0, 8,
                SCRAMBLE_LENGTH_323, SCRAMBLE_LENGTH
    'thd' and 'mqh' are updated on success; other params are IN.
  
  RETURN VALUE
    0  success: thd->priv_user, thd->priv_host, thd->master_access, mqh are
       updated
    1  user not found or authentication failure
    2  user found, has long (4.1.1) salt, but passwd is in old (3.23) format.
   -1  user found, has short (3.23) salt, but passwd is in new (4.1.1) format.
*/

int acl_getroot(THD *thd, USER_RESOURCES  *mqh,
                const char *passwd, uint passwd_len)
{
  ulong user_access= NO_ACCESS;
  int res= 1;
  ACL_USER *acl_user= 0;
  DBUG_ENTER("acl_getroot");

  if (!initialized)
  {
    /* 
      here if mysqld's been started with --skip-grant-tables option.
    */
    thd->priv_user= (char *) "";                // privileges for
    *thd->priv_host= '\0';                      // the user are unknown
    thd->master_access= ~NO_ACCESS;             // everything is allowed
    bzero((char*) mqh, sizeof(*mqh));
    DBUG_RETURN(0);
  }

  VOID(pthread_mutex_lock(&acl_cache->lock));

  /*
    Find acl entry in user database. Note, that find_acl_user is not the same,
    because it doesn't take into account the case when user is not empty,
    but acl_user->user is empty
  */

  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user_tmp= dynamic_element(&acl_users,i,ACL_USER*);
    if (!acl_user_tmp->user || !strcmp(thd->user, acl_user_tmp->user))
    {
      if (compare_hostname(&acl_user_tmp->host, thd->host, thd->ip))
      {
        /* check password: it should be empty or valid */
        if (passwd_len == acl_user_tmp->salt_len)
        {
          if (acl_user_tmp->salt_len == 0 ||
              (acl_user_tmp->salt_len == SCRAMBLE_LENGTH ?
              check_scramble(passwd, thd->scramble, acl_user_tmp->salt) :
              check_scramble_323(passwd, thd->scramble,
                                 (ulong *) acl_user_tmp->salt)) == 0)
          {
            acl_user= acl_user_tmp;
            res= 0;
          }
        }
        else if (passwd_len == SCRAMBLE_LENGTH &&
                 acl_user_tmp->salt_len == SCRAMBLE_LENGTH_323)
          res= -1;
        else if (passwd_len == SCRAMBLE_LENGTH_323 &&
                 acl_user_tmp->salt_len == SCRAMBLE_LENGTH)
          res= 2;
        /* linear search complete: */
        break;
      }
    }
  }
  /*
    This was moved to separate tree because of heavy HAVE_OPENSSL case.
    If acl_user is not null, res is 0.
  */

  if (acl_user)
  {
    /* OK. User found and password checked continue validation */
#ifdef HAVE_OPENSSL
    Vio *vio=thd->net.vio;
    SSL *ssl= (SSL*) vio->ssl_arg;
#endif

    /*
      At this point we know that user is allowed to connect
      from given host by given username/password pair. Now
      we check if SSL is required, if user is using SSL and
      if X509 certificate attributes are OK
    */
    switch (acl_user->ssl_type) {
    case SSL_TYPE_NOT_SPECIFIED:		// Impossible
    case SSL_TYPE_NONE:				// SSL is not required
      user_access= acl_user->access;
      break;
#ifdef HAVE_OPENSSL
    case SSL_TYPE_ANY:				// Any kind of SSL is ok
      if (vio_type(vio) == VIO_TYPE_SSL)
	user_access= acl_user->access;
      break;
    case SSL_TYPE_X509: /* Client should have any valid certificate. */
      /*
	Connections with non-valid certificates are dropped already
	in sslaccept() anyway, so we do not check validity here.

	We need to check for absence of SSL because without SSL
	we should reject connection.
      */
      if (vio_type(vio) == VIO_TYPE_SSL &&
	  SSL_get_verify_result(ssl) == X509_V_OK &&
	  SSL_get_peer_certificate(ssl))
	user_access= acl_user->access;
      break;
    case SSL_TYPE_SPECIFIED: /* Client should have specified attrib */
      /*
	We do not check for absence of SSL because without SSL it does
	not pass all checks here anyway.
	If cipher name is specified, we compare it to actual cipher in
	use.
      */
      X509 *cert;
      if (vio_type(vio) != VIO_TYPE_SSL ||
	  SSL_get_verify_result(ssl) != X509_V_OK)
	break;
      if (acl_user->ssl_cipher)
      {
	DBUG_PRINT("info",("comparing ciphers: '%s' and '%s'",
			   acl_user->ssl_cipher,SSL_get_cipher(ssl)));
	if (!strcmp(acl_user->ssl_cipher,SSL_get_cipher(ssl)))
	  user_access= acl_user->access;
	else
	{
	  if (global_system_variables.log_warnings)
	    sql_print_information("X509 ciphers mismatch: should be '%s' but is '%s'",
			      acl_user->ssl_cipher,
			      SSL_get_cipher(ssl));
	  break;
	}
      }
      /* Prepare certificate (if exists) */
      DBUG_PRINT("info",("checkpoint 1"));
      if (!(cert= SSL_get_peer_certificate(ssl)))
      {
	user_access=NO_ACCESS;
	break;
      }
      DBUG_PRINT("info",("checkpoint 2"));
      /* If X509 issuer is specified, we check it... */
      if (acl_user->x509_issuer)
      {
        DBUG_PRINT("info",("checkpoint 3"));
	char *ptr = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
	DBUG_PRINT("info",("comparing issuers: '%s' and '%s'",
			   acl_user->x509_issuer, ptr));
        if (strcmp(acl_user->x509_issuer, ptr))
        {
          if (global_system_variables.log_warnings)
            sql_print_information("X509 issuer mismatch: should be '%s' "
			      "but is '%s'", acl_user->x509_issuer, ptr);
          free(ptr);
          break;
        }
        user_access= acl_user->access;
        free(ptr);
      }
      DBUG_PRINT("info",("checkpoint 4"));
      /* X509 subject is specified, we check it .. */
      if (acl_user->x509_subject)
      {
        char *ptr= X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        DBUG_PRINT("info",("comparing subjects: '%s' and '%s'",
                           acl_user->x509_subject, ptr));
        if (strcmp(acl_user->x509_subject,ptr))
        {
          if (global_system_variables.log_warnings)
            sql_print_information("X509 subject mismatch: '%s' vs '%s'",
                            acl_user->x509_subject, ptr);
        }
        else
          user_access= acl_user->access;
        free(ptr);
      }
      break;
#else  /* HAVE_OPENSSL */
    default:
      /*
        If we don't have SSL but SSL is required for this user the 
        authentication should fail.
      */
      break;
#endif /* HAVE_OPENSSL */
    }
    thd->master_access= user_access;
    thd->priv_user= acl_user->user ? thd->user : (char *) "";
    *mqh= acl_user->user_resource;

    if (acl_user->host.hostname)
      strmake(thd->priv_host, acl_user->host.hostname, MAX_HOSTNAME);
    else
      *thd->priv_host= 0;
  }
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  DBUG_RETURN(res);
}


/*
 * This is like acl_getroot() above, but it doesn't check password,
 * and we don't care about the user resources.
 * Used to get access rights for SQL SECURITY DEFINER invocation of
 * stored procedures.
 */
int acl_getroot_no_password(THD *thd)
{
  ulong user_access= NO_ACCESS;
  int res= 1;
  uint i;
  ACL_USER *acl_user= 0;
  DBUG_ENTER("acl_getroot_no_password");

  if (!initialized)
  {
    /* 
      here if mysqld's been started with --skip-grant-tables option.
    */
    thd->priv_user= (char *) "";                // privileges for
    *thd->priv_host= '\0';                      // the user are unknown
    thd->master_access= ~NO_ACCESS;             // everything is allowed
    DBUG_RETURN(0);
  }

  VOID(pthread_mutex_lock(&acl_cache->lock));

  thd->master_access= 0;
  thd->db_access= 0;

  /*
     Find acl entry in user database.
     This is specially tailored to suit the check we do for CALL of
     a stored procedure; thd->user is set to what is actually a
     priv_user, which can be ''.
  */
  for (i=0 ; i < acl_users.elements ; i++)
  {
    acl_user= dynamic_element(&acl_users,i,ACL_USER*);
    if ((!acl_user->user && (!thd->user || !thd->user[0])) ||
	(acl_user->user && strcmp(thd->user, acl_user->user) == 0))
    {
      if (compare_hostname(&acl_user->host, thd->host, thd->ip))
      {
	res= 0;
	break;
      }
    }
  }

  if (acl_user)
  {
    for (i=0 ; i < acl_dbs.elements ; i++)
    {
      ACL_DB *acl_db= dynamic_element(&acl_dbs, i, ACL_DB*);
      if (!acl_db->user ||
	  (thd->user && thd->user[0] && !strcmp(thd->user, acl_db->user)))
      {
	if (compare_hostname(&acl_db->host, thd->host, thd->ip))
	{
	  if (!acl_db->db || (thd->db && !strcmp(acl_db->db, thd->db)))
	  {
	    thd->db_access= acl_db->access;
	    break;
	  }
	}
      }
    }
    thd->master_access= acl_user->access;
    thd->priv_user= acl_user->user ? thd->user : (char *) "";

    if (acl_user->host.hostname)
      strmake(thd->priv_host, acl_user->host.hostname, MAX_HOSTNAME);
    else
      *thd->priv_host= 0;
  }
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  DBUG_RETURN(res);
}

static byte* check_get_key(ACL_USER *buff,uint *length,
			   my_bool not_used __attribute__((unused)))
{
  *length=buff->hostname_length;
  return (byte*) buff->host.hostname;
}


static void acl_update_user(const char *user, const char *host,
			    const char *password, uint password_len,
			    enum SSL_type ssl_type,
			    const char *ssl_cipher,
			    const char *x509_issuer,
			    const char *x509_subject,
			    USER_RESOURCES  *mqh,
			    ulong privileges)
{
  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
    if (!acl_user->user && !user[0] ||
	acl_user->user &&
	!strcmp(user,acl_user->user))
    {
      if (!acl_user->host.hostname && !host[0] ||
	  acl_user->host.hostname &&
	  !my_strcasecmp(system_charset_info, host, acl_user->host.hostname))
      {
	acl_user->access=privileges;
	if (mqh->bits & 1)
	  acl_user->user_resource.questions=mqh->questions;
	if (mqh->bits & 2)
	  acl_user->user_resource.updates=mqh->updates;
	if (mqh->bits & 4)
	  acl_user->user_resource.connections=mqh->connections;
	if (ssl_type != SSL_TYPE_NOT_SPECIFIED)
	{
	  acl_user->ssl_type= ssl_type;
	  acl_user->ssl_cipher= (ssl_cipher ? strdup_root(&mem,ssl_cipher) :
				 0);
	  acl_user->x509_issuer= (x509_issuer ? strdup_root(&mem,x509_issuer) :
				  0);
	  acl_user->x509_subject= (x509_subject ?
				   strdup_root(&mem,x509_subject) : 0);
	}
	if (password)
	  set_user_salt(acl_user, password, password_len);
        /* search complete: */
	break;
      }
    }
  }
}


static void acl_insert_user(const char *user, const char *host,
			    const char *password, uint password_len,
			    enum SSL_type ssl_type,
			    const char *ssl_cipher,
			    const char *x509_issuer,
			    const char *x509_subject,
			    USER_RESOURCES *mqh,
			    ulong privileges)
{
  ACL_USER acl_user;
  acl_user.user=*user ? strdup_root(&mem,user) : 0;
  update_hostname(&acl_user.host, *host ? strdup_root(&mem, host): 0);
  acl_user.access=privileges;
  acl_user.user_resource = *mqh;
  acl_user.sort=get_sort(2,acl_user.host.hostname,acl_user.user);
  acl_user.hostname_length=(uint) strlen(host);
  acl_user.ssl_type= (ssl_type != SSL_TYPE_NOT_SPECIFIED ?
		      ssl_type : SSL_TYPE_NONE);
  acl_user.ssl_cipher=	ssl_cipher   ? strdup_root(&mem,ssl_cipher) : 0;
  acl_user.x509_issuer= x509_issuer  ? strdup_root(&mem,x509_issuer) : 0;
  acl_user.x509_subject=x509_subject ? strdup_root(&mem,x509_subject) : 0;

  set_user_salt(&acl_user, password, password_len);

  VOID(push_dynamic(&acl_users,(gptr) &acl_user));
  if (!acl_user.host.hostname || acl_user.host.hostname[0] == wild_many
      && !acl_user.host.hostname[1])
    allow_all_hosts=1;		// Anyone can connect /* purecov: tested */
  qsort((gptr) dynamic_element(&acl_users,0,ACL_USER*),acl_users.elements,
	sizeof(ACL_USER),(qsort_cmp) acl_compare);

  /* We must free acl_check_hosts as its memory is mapped to acl_user */
  delete_dynamic(&acl_wild_hosts);
  hash_free(&acl_check_hosts);
  init_check_host();
}


static void acl_update_db(const char *user, const char *host, const char *db,
			  ulong privileges)
{
  for (uint i=0 ; i < acl_dbs.elements ; i++)
  {
    ACL_DB *acl_db=dynamic_element(&acl_dbs,i,ACL_DB*);
    if (!acl_db->user && !user[0] ||
	acl_db->user &&
	!strcmp(user,acl_db->user))
    {
      if (!acl_db->host.hostname && !host[0] ||
	  acl_db->host.hostname &&
	  !my_strcasecmp(system_charset_info, host, acl_db->host.hostname))
      {
	if (!acl_db->db && !db[0] ||
	    acl_db->db && !strcmp(db,acl_db->db))
	{
	  if (privileges)
	    acl_db->access=privileges;
	  else
	    delete_dynamic_element(&acl_dbs,i);
	}
      }
    }
  }
}


/*
  Insert a user/db/host combination into the global acl_cache

  SYNOPSIS
    acl_insert_db()
    user		User name
    host		Host name
    db			Database name
    privileges		Bitmap of privileges

  NOTES
    acl_cache->lock must be locked when calling this
*/

static void acl_insert_db(const char *user, const char *host, const char *db,
			  ulong privileges)
{
  ACL_DB acl_db;
  safe_mutex_assert_owner(&acl_cache->lock);
  acl_db.user=strdup_root(&mem,user);
  update_hostname(&acl_db.host,strdup_root(&mem,host));
  acl_db.db=strdup_root(&mem,db);
  acl_db.access=privileges;
  acl_db.sort=get_sort(3,acl_db.host.hostname,acl_db.db,acl_db.user);
  VOID(push_dynamic(&acl_dbs,(gptr) &acl_db));
  qsort((gptr) dynamic_element(&acl_dbs,0,ACL_DB*),acl_dbs.elements,
	sizeof(ACL_DB),(qsort_cmp) acl_compare);
}



/*
  Get privilege for a host, user and db combination
*/

ulong acl_get(const char *host, const char *ip,
              const char *user, const char *db, my_bool db_is_pattern)
{
  ulong host_access,db_access;
  uint i,key_length;
  db_access=0; host_access= ~0;
  char key[ACL_KEY_LENGTH],*tmp_db,*end;
  acl_entry *entry;
  DBUG_ENTER("acl_get");

  VOID(pthread_mutex_lock(&acl_cache->lock));
  end=strmov((tmp_db=strmov(strmov(key, ip ? ip : "")+1,user)+1),db);
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }
  key_length=(uint) (end-key);
  if ((entry=(acl_entry*) acl_cache->search(key,key_length)))
  {
    db_access=entry->access;
    VOID(pthread_mutex_unlock(&acl_cache->lock));
    DBUG_PRINT("exit", ("access: 0x%lx", db_access));
    DBUG_RETURN(db_access);
  }

  /*
    Check if there are some access rights for database and user
  */
  for (i=0 ; i < acl_dbs.elements ; i++)
  {
    ACL_DB *acl_db=dynamic_element(&acl_dbs,i,ACL_DB*);
    if (!acl_db->user || !strcmp(user,acl_db->user))
    {
      if (compare_hostname(&acl_db->host,host,ip))
      {
	if (!acl_db->db || !wild_compare(db,acl_db->db,db_is_pattern))
	{
	  db_access=acl_db->access;
	  if (acl_db->host.hostname)
	    goto exit;				// Fully specified. Take it
	  break; /* purecov: tested */
	}
      }
    }
  }
  if (!db_access)
    goto exit;					// Can't be better

  /*
    No host specified for user. Get hostdata from host table
  */
  host_access=0;				// Host must be found
  for (i=0 ; i < acl_hosts.elements ; i++)
  {
    ACL_HOST *acl_host=dynamic_element(&acl_hosts,i,ACL_HOST*);
    if (compare_hostname(&acl_host->host,host,ip))
    {
      if (!acl_host->db || !wild_compare(db,acl_host->db,db_is_pattern))
      {
	host_access=acl_host->access;		// Fully specified. Take it
	break;
      }
    }
  }
exit:
  /* Save entry in cache for quick retrieval */
  if ((entry= (acl_entry*) malloc(sizeof(acl_entry)+key_length)))
  {
    entry->access=(db_access & host_access);
    entry->length=key_length;
    memcpy((gptr) entry->key,key,key_length);
    acl_cache->add(entry);
  }
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  DBUG_PRINT("exit", ("access: 0x%lx", db_access & host_access));
  DBUG_RETURN(db_access & host_access);
}

/*
  Check if there are any possible matching entries for this host

  NOTES
    All host names without wild cards are stored in a hash table,
    entries with wildcards are stored in a dynamic array
*/

static void init_check_host(void)
{
  DBUG_ENTER("init_check_host");
  VOID(my_init_dynamic_array(&acl_wild_hosts,sizeof(struct acl_host_and_ip),
			  acl_users.elements,1));
  VOID(hash_init(&acl_check_hosts,system_charset_info,acl_users.elements,0,0,
		 (hash_get_key) check_get_key,0,0));
  if (!allow_all_hosts)
  {
    for (uint i=0 ; i < acl_users.elements ; i++)
    {
      ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
      if (strchr(acl_user->host.hostname,wild_many) ||
	  strchr(acl_user->host.hostname,wild_one) ||
	  acl_user->host.ip_mask)
      {						// Has wildcard
	uint j;
	for (j=0 ; j < acl_wild_hosts.elements ; j++)
	{					// Check if host already exists
	  acl_host_and_ip *acl=dynamic_element(&acl_wild_hosts,j,
					       acl_host_and_ip *);
	  if (!my_strcasecmp(system_charset_info,
                             acl_user->host.hostname, acl->hostname))
	    break;				// already stored
	}
	if (j == acl_wild_hosts.elements)	// If new
	  (void) push_dynamic(&acl_wild_hosts,(char*) &acl_user->host);
      }
      else if (!hash_search(&acl_check_hosts,(byte*) &acl_user->host,
			    (uint) strlen(acl_user->host.hostname)))
      {
	if (my_hash_insert(&acl_check_hosts,(byte*) acl_user))
	{					// End of memory
	  allow_all_hosts=1;			// Should never happen
	  DBUG_VOID_RETURN;
	}
      }
    }
  }
  freeze_size(&acl_wild_hosts);
  freeze_size(&acl_check_hosts.array);
  DBUG_VOID_RETURN;
}


/* Return true if there is no users that can match the given host */

bool acl_check_host(const char *host, const char *ip)
{
  if (allow_all_hosts)
    return 0;
  VOID(pthread_mutex_lock(&acl_cache->lock));

  if (host && hash_search(&acl_check_hosts,(byte*) host,(uint) strlen(host)) ||
      ip && hash_search(&acl_check_hosts,(byte*) ip,(uint) strlen(ip)))
  {
    VOID(pthread_mutex_unlock(&acl_cache->lock));
    return 0;					// Found host
  }
  for (uint i=0 ; i < acl_wild_hosts.elements ; i++)
  {
    acl_host_and_ip *acl=dynamic_element(&acl_wild_hosts,i,acl_host_and_ip*);
    if (compare_hostname(acl, host, ip))
    {
      VOID(pthread_mutex_unlock(&acl_cache->lock));
      return 0;					// Host ok
    }
  }
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  return 1;					// Host is not allowed
}


/*
  Check if the user is allowed to change password

  SYNOPSIS:
    check_change_password()
    thd		THD
    host	hostname for the user
    user	user name

    RETURN VALUE
      0		OK
      1		ERROR  ; In this case the error is sent to the client.
*/

bool check_change_password(THD *thd, const char *host, const char *user,
                           char *new_password)
{
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    return(1);
  }
  if (!thd->slave_thread &&
      (strcmp(thd->user,user) ||
       my_strcasecmp(system_charset_info, host, thd->host_or_ip)))
  {
    if (check_access(thd, UPDATE_ACL, "mysql",0,1,0))
      return(1);
  }
  if (!thd->slave_thread && !thd->user[0])
  {
    my_message(ER_PASSWORD_ANONYMOUS_USER, ER(ER_PASSWORD_ANONYMOUS_USER),
               MYF(0));
    return(1);
  }
  uint len=strlen(new_password);
  if (len && len != SCRAMBLED_PASSWORD_CHAR_LENGTH &&
      len != SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
  {
    my_error(ER_PASSWD_LENGTH, MYF(0), SCRAMBLED_PASSWORD_CHAR_LENGTH);
    return -1;
  }
  return(0);
}


/*
  Change a password for a user

  SYNOPSIS
    change_password()
    thd			Thread handle
    host		Hostname
    user		User name
    new_password	New password for host@user

  RETURN VALUES
    0	ok
    1	ERROR; In this case the error is sent to the client.
*/

bool change_password(THD *thd, const char *host, const char *user,
		     char *new_password)
{
  DBUG_ENTER("change_password");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'  new_password: '%s'",
		      host,user,new_password));
  DBUG_ASSERT(host != 0);			// Ensured by parent

  if (check_change_password(thd, host, user, new_password))
    DBUG_RETURN(1);

  VOID(pthread_mutex_lock(&acl_cache->lock));
  ACL_USER *acl_user;
  if (!(acl_user= find_acl_user(host, user)))
  {
    VOID(pthread_mutex_unlock(&acl_cache->lock));
    my_message(ER_PASSWORD_NO_MATCH, ER(ER_PASSWORD_NO_MATCH), MYF(0));
    DBUG_RETURN(1);
  }
  /* update loaded acl entry: */
  uint new_password_len= new_password ? strlen(new_password) : 0;
  set_user_salt(acl_user, new_password, new_password_len);

  if (update_user_table(thd,
			acl_user->host.hostname ? acl_user->host.hostname : "",
			acl_user->user ? acl_user->user : "",
			new_password, new_password_len))
  {
    VOID(pthread_mutex_unlock(&acl_cache->lock)); /* purecov: deadcode */
    DBUG_RETURN(1); /* purecov: deadcode */
  }

  acl_cache->clear(1);				// Clear locked hostname cache
  VOID(pthread_mutex_unlock(&acl_cache->lock));

  char buff[512]; /* Extend with extended password length*/
  ulong query_length=
    my_sprintf(buff,
	       (buff,"SET PASSWORD FOR \"%-.120s\"@\"%-.120s\"=\"%-.120s\"",
		acl_user->user ? acl_user->user : "",
		acl_user->host.hostname ? acl_user->host.hostname : "",
		new_password));
  thd->clear_error();
  Query_log_event qinfo(thd, buff, query_length, 0);
  mysql_bin_log.write(&qinfo);
  DBUG_RETURN(0);
}


/*
  Find first entry that matches the current user
*/

static ACL_USER *
find_acl_user(const char *host, const char *user)
{
  DBUG_ENTER("find_acl_user");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'",host,user));
  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
    DBUG_PRINT("info",("strcmp('%s','%s'), compare_hostname('%s','%s'),",
		       user,
		       acl_user->user ? acl_user->user : "",
		       host,
		       acl_user->host.hostname ? acl_user->host.hostname :
		       ""));
    if (!acl_user->user && !user[0] ||
	acl_user->user && !strcmp(user,acl_user->user))
    {
      if (compare_hostname(&acl_user->host,host,host))
      {
	DBUG_RETURN(acl_user);
      }
    }
  }
  DBUG_RETURN(0);
}


/*
  Comparing of hostnames

  NOTES
  A hostname may be of type:
  hostname   (May include wildcards);   monty.pp.sci.fi
  ip	   (May include wildcards);   192.168.0.0
  ip/netmask			      192.168.0.0/255.255.255.0

  A net mask of 0.0.0.0 is not allowed.
*/

static const char *calc_ip(const char *ip, long *val, char end)
{
  long ip_val,tmp;
  if (!(ip=str2int(ip,10,0,255,&ip_val)) || *ip != '.')
    return 0;
  ip_val<<=24;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != '.')
    return 0;
  ip_val+=tmp<<16;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != '.')
    return 0;
  ip_val+=tmp<<8;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != end)
    return 0;
  *val=ip_val+tmp;
  return ip;
}


static void update_hostname(acl_host_and_ip *host, const char *hostname)
{
  host->hostname=(char*) hostname;		// This will not be modified!
  if (!hostname ||
      (!(hostname=calc_ip(hostname,&host->ip,'/')) ||
       !(hostname=calc_ip(hostname+1,&host->ip_mask,'\0'))))
  {
    host->ip= host->ip_mask=0;			// Not a masked ip
  }
}


static bool compare_hostname(const acl_host_and_ip *host, const char *hostname,
			     const char *ip)
{
  long tmp;
  if (host->ip_mask && ip && calc_ip(ip,&tmp,'\0'))
  {
    return (tmp & host->ip_mask) == host->ip;
  }
  return (!host->hostname ||
	  (hostname && !wild_case_compare(system_charset_info,
                                          hostname,host->hostname)) ||
	  (ip && !wild_compare(ip,host->hostname,0)));
}

bool hostname_requires_resolving(const char *hostname)
{
  char cur;
  if (!hostname)
    return FALSE;
  int namelen= strlen(hostname);
  int lhlen= strlen(my_localhost);
  if ((namelen == lhlen) &&
      !my_strnncoll(system_charset_info, (const uchar *)hostname,  namelen,
		    (const uchar *)my_localhost, strlen(my_localhost)))
    return FALSE;
  for (; (cur=*hostname); hostname++)
  {
    if ((cur != '%') && (cur != '_') && (cur != '.') &&
	((cur < '0') || (cur > '9')))
      return TRUE;
  }
  return FALSE;
}

/*
  Update grants in the user and database privilege tables
*/

static bool update_user_table(THD *thd, const char *host, const char *user,
			      const char *new_password, uint new_password_len)
{
  TABLE_LIST tables;
  TABLE *table;
  bool error=1;
  DBUG_ENTER("update_user_table");
  DBUG_PRINT("enter",("user: %s  host: %s",user,host));

  bzero((char*) &tables,sizeof(tables));
  tables.alias=tables.real_name=(char*) "user";
  tables.db=(char*) "mysql";

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && table_rules_on)
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.  It's ok to leave 'updating' set after tables_ok.
    */
    tables.updating= 1;
    /* Thanks to bzero, tables.next==0 */
    if (!tables_ok(0, &tables))
      DBUG_RETURN(0);
  }
#endif

  if (!(table=open_ltable(thd,&tables,TL_WRITE)))
    DBUG_RETURN(1); /* purecov: deadcode */
  table->field[0]->store(host,(uint) strlen(host), system_charset_info);
  table->field[1]->store(user,(uint) strlen(user), system_charset_info);

  table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
  if (table->file->index_read_idx(table->record[0],0,
				  (byte*) table->field[0]->ptr,
				  table->key_info[0].key_length,
				  HA_READ_KEY_EXACT))
  {
    my_message(ER_PASSWORD_NO_MATCH, ER(ER_PASSWORD_NO_MATCH),
               MYF(0));	/* purecov: deadcode */
    DBUG_RETURN(1);				/* purecov: deadcode */
  }
  store_record(table,record[1]);
  table->field[2]->store(new_password, new_password_len, system_charset_info);
  if ((error=table->file->update_row(table->record[1],table->record[0])))
  {
    table->file->print_error(error,MYF(0));	/* purecov: deadcode */
    goto end;					/* purecov: deadcode */
  }
  error=0;					// Record updated

end:
  close_thread_tables(thd);
  DBUG_RETURN(error);
}


/* Return 1 if we are allowed to create new users */

static bool test_if_create_new_users(THD *thd)
{
  bool create_new_users=1;    // Assume that we are allowed to create new users
  if (opt_safe_user_create && !(thd->master_access & INSERT_ACL))
  {
    TABLE_LIST tl;
    ulong db_access;
    bzero((char*) &tl,sizeof(tl));
    tl.db=	   (char*) "mysql";
    tl.real_name=  (char*) "user";

    db_access=acl_get(thd->host, thd->ip,
		      thd->priv_user, tl.db, 0);
    if (!(db_access & INSERT_ACL))
    {
      if (check_grant(thd, INSERT_ACL, &tl, 0, UINT_MAX, 1))
	create_new_users=0;
    }
  }
  return create_new_users;
}


/****************************************************************************
  Handle GRANT commands
****************************************************************************/

static int replace_user_table(THD *thd, TABLE *table, const LEX_USER &combo,
			      ulong rights, bool revoke_grant,
			      bool create_user)
{
  int error = -1;
  bool old_row_exists=0;
  const char *password= "";
  uint password_len= 0;
  char what= (revoke_grant) ? 'N' : 'Y';
  DBUG_ENTER("replace_user_table");
  LEX *lex= thd->lex;
  safe_mutex_assert_owner(&acl_cache->lock);

  if (combo.password.str && combo.password.str[0])
  {
    if (combo.password.length != SCRAMBLED_PASSWORD_CHAR_LENGTH &&
        combo.password.length != SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
    {
      my_error(ER_PASSWD_LENGTH, MYF(0), SCRAMBLED_PASSWORD_CHAR_LENGTH);
      DBUG_RETURN(-1);
    }
    password_len= combo.password.length;
    password=combo.password.str;
  }

  table->field[0]->store(combo.host.str,combo.host.length, system_charset_info);
  table->field[1]->store(combo.user.str,combo.user.length, system_charset_info);
  table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
  if (table->file->index_read_idx(table->record[0], 0,
				  (byte*) table->field[0]->ptr,
				  table->key_info[0].key_length,
				  HA_READ_KEY_EXACT))
  {
    /* what == 'N' means revoke */
    if (what == 'N')
    {
      my_error(ER_NONEXISTING_GRANT, MYF(0), combo.user.str, combo.host.str);
      goto end;
    }
    /*
      There are four options which affect the process of creation of 
      a new user(mysqld option --safe-create-user, 'insert' privilege
      on 'mysql.user' table, using 'GRANT' with 'IDENTIFIED BY' and
      SQL_MODE flag NO_AUTO_CREATE_USER). Below is the simplified rule
      how it should work.
      if (safe-user-create && ! INSERT_priv) => reject
      else if (identified_by) => create
      else if (no_auto_create_user) => reject
      else create
    */
    else if (((thd->variables.sql_mode & MODE_NO_AUTO_CREATE_USER) &&
              !password_len) || !create_user)
    {
      my_error(ER_NO_PERMISSION_TO_CREATE_USER, MYF(0),
               thd->user, thd->host_or_ip);
      goto end;
    }
    old_row_exists = 0;
    restore_record(table,default_values);       // cp empty row from default_values
    table->field[0]->store(combo.host.str,combo.host.length,
                           system_charset_info);
    table->field[1]->store(combo.user.str,combo.user.length,
                           system_charset_info);
    table->field[2]->store(password, password_len,
                           system_charset_info);
  }
  else
  {
    /*
      Check that the user isn't trying to change a password for another
      user if he doesn't have UPDATE privilege to the MySQL database
    */
    DBUG_ASSERT(combo.host.str);
    if (thd->user && combo.password.str &&
        (strcmp(thd->user,combo.user.str) ||
         my_strcasecmp(system_charset_info,
                       combo.host.str, thd->host_or_ip)) &&
        check_access(thd, UPDATE_ACL, "mysql",0,1,0))
      goto end;
    old_row_exists = 1;
    store_record(table,record[1]);			// Save copy for update
    if (combo.password.str)			// If password given
      table->field[2]->store(password, password_len, system_charset_info);
    else if (!rights && !revoke_grant &&
             lex->ssl_type == SSL_TYPE_NOT_SPECIFIED && !lex->mqh.bits)
    {
      DBUG_RETURN(0);
    }
  }

  /* Update table columns with new privileges */

  Field **tmp_field;
  ulong priv;
  for (tmp_field= table->field+3, priv = SELECT_ACL;
       *tmp_field && (*tmp_field)->real_type() == FIELD_TYPE_ENUM &&
	 ((Field_enum*) (*tmp_field))->typelib->count == 2 ;
       tmp_field++, priv <<= 1)
  {
    if (priv & rights)				 // set requested privileges
      (*tmp_field)->store(&what, 1, &my_charset_latin1);
  }
  rights=get_access(table,3);
  DBUG_PRINT("info",("table->fields: %d",table->fields));
  if (table->fields >= 31)		/* From 4.0.0 we have more fields */
  {
    /* We write down SSL related ACL stuff */
    switch (lex->ssl_type) {
    case SSL_TYPE_ANY:
      table->field[24]->store("ANY",3, &my_charset_latin1);
      table->field[25]->store("", 0, &my_charset_latin1);
      table->field[26]->store("", 0, &my_charset_latin1);
      table->field[27]->store("", 0, &my_charset_latin1);
      break;
    case SSL_TYPE_X509:
      table->field[24]->store("X509",4, &my_charset_latin1);
      table->field[25]->store("", 0, &my_charset_latin1);
      table->field[26]->store("", 0, &my_charset_latin1);
      table->field[27]->store("", 0, &my_charset_latin1);
      break;
    case SSL_TYPE_SPECIFIED:
      table->field[24]->store("SPECIFIED",9, &my_charset_latin1);
      table->field[25]->store("", 0, &my_charset_latin1);
      table->field[26]->store("", 0, &my_charset_latin1);
      table->field[27]->store("", 0, &my_charset_latin1);
      if (lex->ssl_cipher)
	table->field[25]->store(lex->ssl_cipher,
				strlen(lex->ssl_cipher), system_charset_info);
      if (lex->x509_issuer)
	table->field[26]->store(lex->x509_issuer,
				strlen(lex->x509_issuer), system_charset_info);
      if (lex->x509_subject)
	table->field[27]->store(lex->x509_subject,
				strlen(lex->x509_subject), system_charset_info);
      break;
    case SSL_TYPE_NOT_SPECIFIED:
      break;
    case SSL_TYPE_NONE:
      table->field[24]->store("", 0, &my_charset_latin1);
      table->field[25]->store("", 0, &my_charset_latin1);
      table->field[26]->store("", 0, &my_charset_latin1);
      table->field[27]->store("", 0, &my_charset_latin1);
      break;
    }

    USER_RESOURCES mqh= lex->mqh;
    if (mqh.bits & 1)
      table->field[28]->store((longlong) mqh.questions);
    if (mqh.bits & 2)
      table->field[29]->store((longlong) mqh.updates);
    if (mqh.bits & 4)
      table->field[30]->store((longlong) mqh.connections);
    mqh_used = mqh_used || mqh.questions || mqh.updates || mqh.connections;
  }
  if (old_row_exists)
  {
    /*
      We should NEVER delete from the user table, as a uses can still
      use mysqld even if he doesn't have any privileges in the user table!
    */
    table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
    if (cmp_record(table,record[1]) &&
	(error=table->file->update_row(table->record[1],table->record[0])))
    {						// This should never happen
      table->file->print_error(error,MYF(0));	/* purecov: deadcode */
      error= -1;				/* purecov: deadcode */
      goto end;					/* purecov: deadcode */
    }
  }
  else if ((error=table->file->write_row(table->record[0]))) // insert
  {						// This should never happen
    if (error && error != HA_ERR_FOUND_DUPP_KEY &&
	error != HA_ERR_FOUND_DUPP_UNIQUE)	/* purecov: inspected */
    {
      table->file->print_error(error,MYF(0));	/* purecov: deadcode */
      error= -1;				/* purecov: deadcode */
      goto end;					/* purecov: deadcode */
    }
  }
  error=0;					// Privileges granted / revoked

end:
  if (!error)
  {
    acl_cache->clear(1);			// Clear privilege cache
    if (old_row_exists)
      acl_update_user(combo.user.str, combo.host.str,
                      combo.password.str, password_len,
		      lex->ssl_type,
		      lex->ssl_cipher,
		      lex->x509_issuer,
		      lex->x509_subject,
		      &lex->mqh,
		      rights);
    else
      acl_insert_user(combo.user.str, combo.host.str, password, password_len,
		      lex->ssl_type,
		      lex->ssl_cipher,
		      lex->x509_issuer,
		      lex->x509_subject,
		      &lex->mqh,
		      rights);
  }
  DBUG_RETURN(error);
}


/*
  change grants in the mysql.db table
*/

static int replace_db_table(TABLE *table, const char *db,
			    const LEX_USER &combo,
			    ulong rights, bool revoke_grant)
{
  uint i;
  ulong priv,store_rights;
  bool old_row_exists=0;
  int error;
  char what= (revoke_grant) ? 'N' : 'Y';
  DBUG_ENTER("replace_db_table");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(-1);
  }

  /* Check if there is such a user in user table in memory? */
  if (!find_acl_user(combo.host.str,combo.user.str))
  {
    my_message(ER_PASSWORD_NO_MATCH, ER(ER_PASSWORD_NO_MATCH), MYF(0));
    DBUG_RETURN(-1);
  }

  table->field[0]->store(combo.host.str,combo.host.length, system_charset_info);
  table->field[1]->store(db,(uint) strlen(db), system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length, system_charset_info);
  table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
  if (table->file->index_read_idx(table->record[0],0,
				  (byte*) table->field[0]->ptr,
				  table->key_info[0].key_length,
				  HA_READ_KEY_EXACT))
  {
    if (what == 'N')
    { // no row, no revoke
      my_error(ER_NONEXISTING_GRANT, MYF(0), combo.user.str, combo.host.str);
      goto abort;
    }
    old_row_exists = 0;
    restore_record(table,default_values);			// cp empty row from default_values
    table->field[0]->store(combo.host.str,combo.host.length, system_charset_info);
    table->field[1]->store(db,(uint) strlen(db), system_charset_info);
    table->field[2]->store(combo.user.str,combo.user.length, system_charset_info);
  }
  else
  {
    old_row_exists = 1;
    store_record(table,record[1]);
  }

  store_rights=get_rights_for_db(rights);
  for (i= 3, priv= 1; i < table->fields; i++, priv <<= 1)
  {
    if (priv & store_rights)			// do it if priv is chosen
      table->field [i]->store(&what,1, &my_charset_latin1);// set requested privileges
  }
  rights=get_access(table,3);
  rights=fix_rights_for_db(rights);

  if (old_row_exists)
  {
    /* update old existing row */
    if (rights)
    {
      table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
      if ((error=table->file->update_row(table->record[1],table->record[0])))
	goto table_error;			/* purecov: deadcode */
    }
    else	/* must have been a revoke of all privileges */
    {
      if ((error = table->file->delete_row(table->record[1])))
	goto table_error;			/* purecov: deadcode */
    }
  }
  else if (rights && (error=table->file->write_row(table->record[0])))
  {
    if (error && error != HA_ERR_FOUND_DUPP_KEY) /* purecov: inspected */
      goto table_error; /* purecov: deadcode */
  }

  acl_cache->clear(1);				// Clear privilege cache
  if (old_row_exists)
    acl_update_db(combo.user.str,combo.host.str,db,rights);
  else
  if (rights)
    acl_insert_db(combo.user.str,combo.host.str,db,rights);
  DBUG_RETURN(0);

  /* This could only happen if the grant tables got corrupted */
table_error:
  table->file->print_error(error,MYF(0));	/* purecov: deadcode */

abort:
  DBUG_RETURN(-1);
}


class GRANT_COLUMN :public Sql_alloc
{
public:
  char *column;
  ulong rights;
  uint key_length;
  GRANT_COLUMN(String &c,  ulong y) :rights (y)
  {
    column= memdup_root(&memex,c.ptr(), key_length=c.length());
  }
};


static byte* get_key_column(GRANT_COLUMN *buff,uint *length,
			    my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (byte*) buff->column;
}


class GRANT_TABLE :public Sql_alloc
{
public:
  char *host,*db, *user, *tname, *hash_key, *orig_host;
  ulong privs, cols;
  ulong sort;
  uint key_length;
  HASH hash_columns;

  GRANT_TABLE(const char *h, const char *d,const char *u,
              const char *t, ulong p, ulong c);
  GRANT_TABLE (TABLE *form, TABLE *col_privs);
  ~GRANT_TABLE();
  bool ok() { return privs != 0 || cols != 0; }
};



GRANT_TABLE::GRANT_TABLE(const char *h, const char *d,const char *u,
                         const char *t, ulong p, ulong c)
  :privs(p), cols(c)
{
  /* Host given by user */
  orig_host=  strdup_root(&memex,h);
  /* Convert empty hostname to '%' for easy comparison */
  host=  orig_host[0] ? orig_host : (char*) "%";
  db =   strdup_root(&memex,d);
  user = strdup_root(&memex,u);
  sort=  get_sort(3,host,db,user);
  tname= strdup_root(&memex,t);
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, db);
    my_casedn_str(files_charset_info, tname);
  }
  key_length =(uint) strlen(d)+(uint) strlen(u)+(uint) strlen(t)+3;
  hash_key = (char*) alloc_root(&memex,key_length);
  strmov(strmov(strmov(hash_key,user)+1,db)+1,tname);
  (void) hash_init(&hash_columns,system_charset_info,
                   0,0,0, (hash_get_key) get_key_column,0,0);
}


GRANT_TABLE::GRANT_TABLE(TABLE *form, TABLE *col_privs)
{
  byte key[MAX_KEY_LENGTH];

  orig_host= host= get_field(&memex, form->field[0]);
  db=    get_field(&memex,form->field[1]);
  user=  get_field(&memex,form->field[2]);
  if (!user)
    user= (char*) "";
  if (!orig_host)
  {
    orig_host= (char*) "";
    host= (char*) "%";
  }
  sort=  get_sort(3, orig_host, db, user);
  tname= get_field(&memex,form->field[3]);
  if (!db || !tname)
  {
    /* Wrong table row; Ignore it */
    hash_clear(&hash_columns);                  /* allow for destruction */
    privs= cols= 0;				/* purecov: inspected */
    return;					/* purecov: inspected */
  }
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, db);
    my_casedn_str(files_charset_info, tname);
  }
  key_length = ((uint) strlen(db) + (uint) strlen(user) +
                (uint) strlen(tname) + 3);
  hash_key = (char*) alloc_root(&memex,key_length);
  strmov(strmov(strmov(hash_key,user)+1,db)+1,tname);
  privs = (ulong) form->field[6]->val_int();
  cols  = (ulong) form->field[7]->val_int();
  privs = fix_rights_for_table(privs);
  cols =  fix_rights_for_column(cols);

  (void) hash_init(&hash_columns,system_charset_info,
                   0,0,0, (hash_get_key) get_key_column,0,0);
  if (cols)
  {
    int key_len;
    col_privs->field[0]->store(orig_host,(uint) strlen(orig_host),
                               system_charset_info);
    col_privs->field[1]->store(db,(uint) strlen(db), system_charset_info);
    col_privs->field[2]->store(user,(uint) strlen(user), system_charset_info);
    col_privs->field[3]->store(tname,(uint) strlen(tname), system_charset_info);
    key_len=(col_privs->field[0]->pack_length()+
             col_privs->field[1]->pack_length()+
             col_privs->field[2]->pack_length()+
             col_privs->field[3]->pack_length());
    key_copy(key,col_privs->record[0],col_privs->key_info,key_len);
    col_privs->field[4]->store("",0, &my_charset_latin1);
    col_privs->file->ha_index_init(0);
    if (col_privs->file->index_read(col_privs->record[0],
                                    (byte*) col_privs->field[0]->ptr,
                                    key_len, HA_READ_KEY_EXACT))
    {
      cols = 0; /* purecov: deadcode */
      col_privs->file->ha_index_end();
      return;
    }
    do
    {
      String *res,column_name;
      GRANT_COLUMN *mem_check;
      /* As column name is a string, we don't have to supply a buffer */
      res=col_privs->field[4]->val_str(&column_name);
      ulong priv= (ulong) col_privs->field[6]->val_int();
      if (!(mem_check = new GRANT_COLUMN(*res,
                                         fix_rights_for_column(priv))))
      {
        /* Don't use this entry */
        privs = cols = 0;			/* purecov: deadcode */
        return;				/* purecov: deadcode */
      }
      my_hash_insert(&hash_columns, (byte *) mem_check);
    } while (!col_privs->file->index_next(col_privs->record[0]) &&
             !key_cmp_if_same(col_privs,key,0,key_len));
    col_privs->file->ha_index_end();
  }
}


GRANT_TABLE::~GRANT_TABLE()
{
  hash_free(&hash_columns);
}


static byte* get_grant_table(GRANT_TABLE *buff,uint *length,
			     my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (byte*) buff->hash_key;
}


void free_grant_table(GRANT_TABLE *grant_table)
{
  hash_free(&grant_table->hash_columns);
}


/* Search after a matching grant. Prefer exact grants before not exact ones */

static GRANT_TABLE *table_hash_search(const char *host,const char* ip,
				      const char *db,
				      const char *user, const char *tname,
				      bool exact)
{
  char helping [NAME_LEN*2+USERNAME_LENGTH+3];
  uint len;
  GRANT_TABLE *grant_table,*found=0;

  len  = (uint) (strmov(strmov(strmov(helping,user)+1,db)+1,tname)-helping)+ 1;
  for (grant_table=(GRANT_TABLE*) hash_search(&column_priv_hash,
					      (byte*) helping,
					      len) ;
       grant_table ;
       grant_table= (GRANT_TABLE*) hash_next(&column_priv_hash,(byte*) helping,
					     len))
  {
    if (exact)
    {
      if ((host &&
	   !my_strcasecmp(system_charset_info, host, grant_table->host)) ||
	  (ip && !strcmp(ip,grant_table->host)))
	return grant_table;
    }
    else
    {
      if (((host && !wild_case_compare(system_charset_info,
				       host,grant_table->host)) ||
	   (ip && !wild_case_compare(system_charset_info,
				     ip,grant_table->host))) &&
          (!found || found->sort < grant_table->sort))
	found=grant_table;					// Host ok
    }
  }
  return found;
}



inline GRANT_COLUMN *
column_hash_search(GRANT_TABLE *t, const char *cname, uint length)
{
  return (GRANT_COLUMN*) hash_search(&t->hash_columns, (byte*) cname,length);
}


static int replace_column_table(GRANT_TABLE *g_t,
				TABLE *table, const LEX_USER &combo,
				List <LEX_COLUMN> &columns,
				const char *db, const char *table_name,
				ulong rights, bool revoke_grant)
{
  int error=0,result=0;
  uint key_length;
  byte key[MAX_KEY_LENGTH];
  DBUG_ENTER("replace_column_table");

  table->field[0]->store(combo.host.str,combo.host.length, system_charset_info);
  table->field[1]->store(db,(uint) strlen(db), system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length, system_charset_info);
  table->field[3]->store(table_name,(uint) strlen(table_name), system_charset_info);
  key_length=(table->field[0]->pack_length()+ table->field[1]->pack_length()+
	      table->field[2]->pack_length()+ table->field[3]->pack_length());
  key_copy(key,table->record[0],table->key_info,key_length);

  rights &= COL_ACLS;				// Only ACL for columns

  /* first fix privileges for all columns in column list */

  List_iterator <LEX_COLUMN> iter(columns);
  class LEX_COLUMN *xx;
  table->file->ha_index_init(0);
  while ((xx=iter++))
  {
    ulong privileges = xx->rights;
    bool old_row_exists=0;
    key_restore(table->record[0],key,table->key_info,key_length);
    table->field[4]->store(xx->column.ptr(),xx->column.length(),
                           system_charset_info);

    table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
    if (table->file->index_read(table->record[0],(byte*) table->field[0]->ptr,
				table->key_info[0].key_length,
				HA_READ_KEY_EXACT))
    {
      if (revoke_grant)
      {
	my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
                 combo.user.str, combo.host.str,
                 table_name); /* purecov: inspected */
	result= -1; /* purecov: inspected */
	continue; /* purecov: inspected */
      }
      old_row_exists = 0;
      restore_record(table,default_values);		// Get empty record
      key_restore(table->record[0],key,table->key_info,key_length);
      table->field[4]->store(xx->column.ptr(),xx->column.length(),
                             system_charset_info);
    }
    else
    {
      ulong tmp= (ulong) table->field[6]->val_int();
      tmp=fix_rights_for_column(tmp);

      if (revoke_grant)
	privileges = tmp & ~(privileges | rights);
      else
	privileges |= tmp;
      old_row_exists = 1;
      store_record(table,record[1]);			// copy original row
    }

    table->field[6]->store((longlong) get_rights_for_column(privileges));

    if (old_row_exists)
    {
      if (privileges)
	error=table->file->update_row(table->record[1],table->record[0]);
      else
	error=table->file->delete_row(table->record[1]);
      if (error)
      {
	table->file->print_error(error,MYF(0)); /* purecov: inspected */
	result= -1;				/* purecov: inspected */
	goto end;				/* purecov: inspected */
      }
      GRANT_COLUMN *grant_column = column_hash_search(g_t,
						      xx->column.ptr(),
						      xx->column.length());
      if (grant_column)				// Should always be true
	grant_column->rights = privileges;	// Update hash
    }
    else					// new grant
    {
      if ((error=table->file->write_row(table->record[0])))
      {
	table->file->print_error(error,MYF(0)); /* purecov: inspected */
	result= -1;				/* purecov: inspected */
	goto end;				/* purecov: inspected */
      }
      GRANT_COLUMN *grant_column = new GRANT_COLUMN(xx->column,privileges);
      my_hash_insert(&g_t->hash_columns,(byte*) grant_column);
    }
  }

  /*
    If revoke of privileges on the table level, remove all such privileges
    for all columns
  */

  if (revoke_grant)
  {
    table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
    if (table->file->index_read(table->record[0], (byte*) table->field[0]->ptr,
				table->key_info[0].key_length,
				HA_READ_KEY_EXACT))
      goto end;

    /* Scan through all rows with the same host,db,user and table */
    do
    {
      ulong privileges = (ulong) table->field[6]->val_int();
      privileges=fix_rights_for_column(privileges);
      store_record(table,record[1]);

      if (privileges & rights)	// is in this record the priv to be revoked ??
      {
	GRANT_COLUMN *grant_column = NULL;
	char  colum_name_buf[HOSTNAME_LENGTH+1];
	String column_name(colum_name_buf,sizeof(colum_name_buf),system_charset_info);

	privileges&= ~rights;
	table->field[6]->store((longlong)
			       get_rights_for_column(privileges));
	table->field[4]->val_str(&column_name);
	grant_column = column_hash_search(g_t,
					  column_name.ptr(),
					  column_name.length());
	if (privileges)
	{
	  int tmp_error;
	  if ((tmp_error=table->file->update_row(table->record[1],
						 table->record[0])))
	  {					/* purecov: deadcode */
	    table->file->print_error(tmp_error,MYF(0)); /* purecov: deadcode */
	    result= -1;				/* purecov: deadcode */
	    goto end;				/* purecov: deadcode */
	  }
	  if (grant_column)
	    grant_column->rights  = privileges; // Update hash
	}
	else
	{
	  int tmp_error;
	  if ((tmp_error = table->file->delete_row(table->record[1])))
	  {					/* purecov: deadcode */
	    table->file->print_error(tmp_error,MYF(0)); /* purecov: deadcode */
	    result= -1;				/* purecov: deadcode */
	    goto end;				/* purecov: deadcode */
	  }
	  if (grant_column)
	    hash_delete(&g_t->hash_columns,(byte*) grant_column);
	}
      }
    } while (!table->file->index_next(table->record[0]) &&
	     !key_cmp_if_same(table,key,0,key_length));
  }

end:
  table->file->ha_index_end();
  DBUG_RETURN(result);
}


static int replace_table_table(THD *thd, GRANT_TABLE *grant_table,
			       TABLE *table, const LEX_USER &combo,
			       const char *db, const char *table_name,
			       ulong rights, ulong col_rights,
			       bool revoke_grant)
{
  char grantor[HOSTNAME_LENGTH+USERNAME_LENGTH+2];
  int old_row_exists = 1;
  int error=0;
  ulong store_table_rights, store_col_rights;
  DBUG_ENTER("replace_table_table");

  strxmov(grantor, thd->user, "@", thd->host_or_ip, NullS);

  /*
    The following should always succeed as new users are created before
    this function is called!
  */
  if (!find_acl_user(combo.host.str,combo.user.str))
  {
    my_message(ER_PASSWORD_NO_MATCH, ER(ER_PASSWORD_NO_MATCH),
               MYF(0));	/* purecov: deadcode */
    DBUG_RETURN(-1);				/* purecov: deadcode */
  }

  restore_record(table,default_values);			// Get empty record
  table->field[0]->store(combo.host.str,combo.host.length, system_charset_info);
  table->field[1]->store(db,(uint) strlen(db), system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length, system_charset_info);
  table->field[3]->store(table_name,(uint) strlen(table_name), system_charset_info);
  store_record(table,record[1]);			// store at pos 1
  table->file->extra(HA_EXTRA_RETRIEVE_ALL_COLS);
  if (table->file->index_read_idx(table->record[0],0,
				  (byte*) table->field[0]->ptr,
				  table->key_info[0].key_length,
				  HA_READ_KEY_EXACT))
  {
    /*
      The following should never happen as we first check the in memory
      grant tables for the user.  There is however always a small change that
      the user has modified the grant tables directly.
    */
    if (revoke_grant)
    { // no row, no revoke
      my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
               combo.user.str, combo.host.str,
               table_name);		        /* purecov: deadcode */
      DBUG_RETURN(-1);				/* purecov: deadcode */
    }
    old_row_exists = 0;
    restore_record(table,record[1]);			// Get saved record
  }

  store_table_rights= get_rights_for_table(rights);
  store_col_rights=   get_rights_for_column(col_rights);
  if (old_row_exists)
  {
    ulong j,k;
    store_record(table,record[1]);
    j = (ulong) table->field[6]->val_int();
    k = (ulong) table->field[7]->val_int();

    if (revoke_grant)
    {
      /* column rights are already fixed in mysql_table_grant */
      store_table_rights=j & ~store_table_rights;
    }
    else
    {
      store_table_rights|= j;
      store_col_rights|=   k;
    }
  }

  table->field[4]->store(grantor,(uint) strlen(grantor), system_charset_info);
  table->field[6]->store((longlong) store_table_rights);
  table->field[7]->store((longlong) store_col_rights);
  rights=fix_rights_for_table(store_table_rights);
  col_rights=fix_rights_for_column(store_col_rights);

  if (old_row_exists)
  {
    if (store_table_rights || store_col_rights)
    {
      if ((error=table->file->update_row(table->record[1],table->record[0])))
	goto table_error;			/* purecov: deadcode */
    }
    else if ((error = table->file->delete_row(table->record[1])))
      goto table_error;				/* purecov: deadcode */
  }
  else
  {
    error=table->file->write_row(table->record[0]);
    if (error && error != HA_ERR_FOUND_DUPP_KEY)
      goto table_error;				/* purecov: deadcode */
  }

  if (rights | col_rights)
  {
    grant_table->privs= rights;
    grant_table->cols=	col_rights;
  }
  else
  {
    hash_delete(&column_priv_hash,(byte*) grant_table);
  }
  DBUG_RETURN(0);

  /* This should never happen */
table_error:
  table->file->print_error(error,MYF(0)); /* purecov: deadcode */
  DBUG_RETURN(-1); /* purecov: deadcode */
}


/*
  Store table level and column level grants in the privilege tables

  SYNOPSIS
    mysql_table_grant()
    thd			Thread handle
    table_list		List of tables to give grant
    user_list		List of users to give grant
    columns		List of columns to give grant
    rights		Table level grant
    revoke_grant	Set to 1 if this is a REVOKE command

  RETURN
    FALSE ok
    TRUE  error
*/

bool mysql_table_grant(THD *thd, TABLE_LIST *table_list,
		      List <LEX_USER> &user_list,
		      List <LEX_COLUMN> &columns, ulong rights,
		      bool revoke_grant)
{
  ulong column_priv= 0;
  List_iterator <LEX_USER> str_list (user_list);
  LEX_USER *Str;
  TABLE_LIST tables[3];
  bool create_new_users=0;
  char *db_name, *real_name;
  DBUG_ENTER("mysql_table_grant");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--skip-grant-tables");	/* purecov: inspected */
    DBUG_RETURN(TRUE);				/* purecov: inspected */
  }
  if (rights & ~TABLE_ACLS)
  {
    my_message(ER_ILLEGAL_GRANT_FOR_TABLE, ER(ER_ILLEGAL_GRANT_FOR_TABLE),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (columns.elements && !revoke_grant)
  {
    class LEX_COLUMN *column;
    List_iterator <LEX_COLUMN> column_iter(columns);
    int res;

    if (open_and_lock_tables(thd, table_list))
      DBUG_RETURN(TRUE);
    
    while ((column = column_iter++))
    {
      uint unused_field_idx= NO_CACHED_FIELD_INDEX;
      if (!find_field_in_table(thd, table_list, column->column.ptr(),
                               column->column.ptr(),
                               column->column.length(), 0, 0, 0, 0,
                               &unused_field_idx, FALSE))
      {
	my_error(ER_BAD_FIELD_ERROR, MYF(0),
                 column->column.c_ptr(), table_list->alias);
	DBUG_RETURN(TRUE);
      }
      column_priv|= column->rights;
    }
    close_thread_tables(thd);
  }
  else if (!(rights & CREATE_ACL) && !revoke_grant)
  {
    char buf[FN_REFLEN];
    sprintf(buf,"%s/%s/%s.frm",mysql_data_home, table_list->db,
	    table_list->real_name);
    fn_format(buf,buf,"","",4+16+32);
    if (access(buf,F_OK))
    {
      my_error(ER_NO_SUCH_TABLE, MYF(0), table_list->db, table_list->alias);
      DBUG_RETURN(TRUE);
    }
  }

  /* open the mysql.tables_priv and mysql.columns_priv tables */

  bzero((char*) &tables,sizeof(tables));
  tables[0].alias=tables[0].real_name= (char*) "user";
  tables[1].alias=tables[1].real_name= (char*) "tables_priv";
  tables[2].alias=tables[2].real_name= (char*) "columns_priv";
  tables[0].next_local= tables[0].next_global= tables+1;
  /* Don't open column table if we don't need it ! */
  tables[1].next_local=
    tables[1].next_global= ((column_priv ||
			     (revoke_grant &&
			      ((rights & COL_ACLS) || columns.elements)))
			    ? tables+2 : 0);
  tables[0].lock_type=tables[1].lock_type=tables[2].lock_type=TL_WRITE;
  tables[0].db=tables[1].db=tables[2].db=(char*) "mysql";

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && table_rules_on)
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating= tables[1].updating= tables[2].updating= 1;
    if (!tables_ok(0, tables))
      DBUG_RETURN(FALSE);
  }
#endif

  if (simple_open_n_lock_tables(thd,tables))
  {						// Should never happen
    close_thread_tables(thd);			/* purecov: deadcode */
    DBUG_RETURN(TRUE);				/* purecov: deadcode */
  }

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);
  bool result= FALSE;
  rw_wrlock(&LOCK_grant);
  MEM_ROOT *old_root= thd->mem_root;
  thd->mem_root= &memex;

  while ((Str = str_list++))
  {
    int error;
    GRANT_TABLE *grant_table;
    if (Str->host.length > HOSTNAME_LENGTH ||
	Str->user.length > USERNAME_LENGTH)
    {
      my_message(ER_GRANT_WRONG_HOST_OR_USER, ER(ER_GRANT_WRONG_HOST_OR_USER),
                 MYF(0));
      result= TRUE;
      continue;
    }
    /* Create user if needed */
    pthread_mutex_lock(&acl_cache->lock);
    error=replace_user_table(thd, tables[0].table, *Str,
			     0, revoke_grant, create_new_users);
    pthread_mutex_unlock(&acl_cache->lock);
    if (error)
    {
      result= TRUE;				// Remember error
      continue;					// Add next user
    }

    db_name= (table_list->view_db.length ?
	      table_list->view_db.str :
	      table_list->db);
    real_name= (table_list->view_name.length ?
		table_list->view_name.str :
		table_list->real_name);

    /* Find/create cached table grant */
    grant_table= table_hash_search(Str->host.str, NullS, db_name,
				   Str->user.str, real_name, 1);
    if (!grant_table)
    {
      if (revoke_grant)
      {
	my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
                 Str->user.str, Str->host.str, table_list->real_name);
	result= TRUE;
	continue;
      }
      grant_table = new GRANT_TABLE (Str->host.str, db_name,
				     Str->user.str, real_name,
				     rights,
				     column_priv);
      if (!grant_table)				// end of memory
      {
	result= TRUE;				/* purecov: deadcode */
	continue;				/* purecov: deadcode */
      }
      my_hash_insert(&column_priv_hash,(byte*) grant_table);
    }

    /* If revoke_grant, calculate the new column privilege for tables_priv */
    if (revoke_grant)
    {
      class LEX_COLUMN *column;
      List_iterator <LEX_COLUMN> column_iter(columns);
      GRANT_COLUMN *grant_column;

      /* Fix old grants */
      while ((column = column_iter++))
      {
	grant_column = column_hash_search(grant_table,
					  column->column.ptr(),
					  column->column.length());
	if (grant_column)
	  grant_column->rights&= ~(column->rights | rights);
      }
      /* scan trough all columns to get new column grant */
      column_priv= 0;
      for (uint idx=0 ; idx < grant_table->hash_columns.records ; idx++)
      {
	grant_column= (GRANT_COLUMN*) hash_element(&grant_table->hash_columns,
						   idx);
	grant_column->rights&= ~rights;		// Fix other columns
	column_priv|= grant_column->rights;
      }
    }
    else
    {
      column_priv|= grant_table->cols;
    }


    /* update table and columns */

    if (replace_table_table(thd, grant_table, tables[1].table, *Str,
			    db_name, real_name,
			    rights, column_priv, revoke_grant))
    {
      /* Should only happen if table is crashed */
      result= TRUE;			       /* purecov: deadcode */
    }
    else if (tables[2].table)
    {
      if ((replace_column_table(grant_table, tables[2].table, *Str,
				columns,
				db_name, real_name,
				rights, revoke_grant)))
      {
	result= TRUE;
      }
    }
  }
  grant_option=TRUE;
  thd->mem_root= old_root;
  rw_unlock(&LOCK_grant);
  if (!result)
    send_ok(thd);
  /* Tables are automatically closed */
  DBUG_RETURN(result);
}


bool mysql_grant(THD *thd, const char *db, List <LEX_USER> &list,
                 ulong rights, bool revoke_grant)
{
  List_iterator <LEX_USER> str_list (list);
  LEX_USER *Str;
  char tmp_db[NAME_LEN+1];
  bool create_new_users=0;
  TABLE_LIST tables[2];
  DBUG_ENTER("mysql_grant");
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--skip-grant-tables");	/* purecov: tested */
    DBUG_RETURN(TRUE);				/* purecov: tested */
  }

  if (lower_case_table_names && db)
  {
    strmov(tmp_db,db);
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }

  /* open the mysql.user and mysql.db tables */
  bzero((char*) &tables,sizeof(tables));
  tables[0].alias=tables[0].real_name=(char*) "user";
  tables[1].alias=tables[1].real_name=(char*) "db";
  tables[0].next_local= tables[0].next_global= tables+1;
  tables[0].lock_type=tables[1].lock_type=TL_WRITE;
  tables[0].db=tables[1].db=(char*) "mysql";

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && table_rules_on)
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating= tables[1].updating= 1;
    if (!tables_ok(0, tables))
      DBUG_RETURN(FALSE);
  }
#endif

  if (simple_open_n_lock_tables(thd,tables))
  {						// This should never happen
    close_thread_tables(thd);			/* purecov: deadcode */
    DBUG_RETURN(TRUE);				/* purecov: deadcode */
  }

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);

  /* go through users in user_list */
  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));
  grant_version++;

  int result=0;
  while ((Str = str_list++))
  {
    if (Str->host.length > HOSTNAME_LENGTH ||
	Str->user.length > USERNAME_LENGTH)
    {
      my_message(ER_GRANT_WRONG_HOST_OR_USER, ER(ER_GRANT_WRONG_HOST_OR_USER),
                 MYF(0));
      result= -1;
      continue;
    }
    if ((replace_user_table(thd,
			    tables[0].table,
			    *Str,
			    (!db ? rights : 0), revoke_grant,
			    create_new_users)))
      result= -1;
    else if (db)
    {
      ulong db_rights= rights & DB_ACLS;
      if (db_rights  == rights)
      {
	if (replace_db_table(tables[1].table, db, *Str, db_rights,
			     revoke_grant))
	  result= -1;
      }
      else
      {
	my_error(ER_WRONG_USAGE, MYF(0), "DB GRANT", "GLOBAL PRIVILEGES");
	result= -1;
      }
    }
  }
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);
  close_thread_tables(thd);

  if (!result)
    send_ok(thd);
  DBUG_RETURN(result);
}


/* Free grant array if possible */

void  grant_free(void)
{
  DBUG_ENTER("grant_free");
  grant_option = FALSE;
  hash_free(&column_priv_hash);
  free_root(&memex,MYF(0));
  DBUG_VOID_RETURN;
}


/* Init grant array if possible */

my_bool grant_init(THD *org_thd)
{
  THD  *thd;
  TABLE_LIST tables[2];
  MYSQL_LOCK *lock;
  MEM_ROOT *memex_ptr;
  my_bool return_val= 1;
  TABLE *t_table, *c_table;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  DBUG_ENTER("grant_init");

  grant_option = FALSE;
  (void) hash_init(&column_priv_hash,system_charset_info,
		   0,0,0, (hash_get_key) get_grant_table,
		   (hash_free_key) free_grant_table,0);
  init_sql_alloc(&memex, ACL_ALLOC_BLOCK_SIZE, 0);

  /* Don't do anything if running with --skip-grant */
  if (!initialized)
    DBUG_RETURN(0);				/* purecov: tested */

  if (!(thd=new THD))
    DBUG_RETURN(1);				/* purecov: deadcode */
  thd->store_globals();
  thd->db= my_strdup("mysql",MYF(0));
  thd->db_length=5;				// Safety
  bzero((char*) &tables, sizeof(tables));
  tables[0].alias=tables[0].real_name= (char*) "tables_priv";
  tables[1].alias=tables[1].real_name= (char*) "columns_priv";
  tables[0].next_local= tables[0].next_global= tables+1;
  tables[0].lock_type=tables[1].lock_type=TL_READ;
  tables[0].db=tables[1].db=thd->db;

  uint counter;
  if (open_tables(thd, tables, &counter))
    goto end;

  TABLE *ptr[2];				// Lock tables for quick update
  ptr[0]= tables[0].table;
  ptr[1]= tables[1].table;
  if (!(lock=mysql_lock_tables(thd,ptr,2)))
    goto end;

  t_table = tables[0].table; c_table = tables[1].table;
  t_table->file->ha_index_init(0);
  if (t_table->file->index_first(t_table->record[0]))
  {
    return_val= 0;
    goto end_unlock;
  }
  grant_option= TRUE;

  /* Will be restored by org_thd->store_globals() */
  memex_ptr= &memex;
  my_pthread_setspecific_ptr(THR_MALLOC, &memex_ptr);
  do
  {
    GRANT_TABLE *mem_check;
    if (!(mem_check=new GRANT_TABLE(t_table,c_table)))
    {
      /* This could only happen if we are out memory */
      grant_option= FALSE;			/* purecov: deadcode */
      goto end_unlock;
    }

    if (check_no_resolve)
    {
      if (hostname_requires_resolving(mem_check->host))
      {
        sql_print_warning("'tables_priv' entry '%s %s@%s' "
                          "ignored in --skip-name-resolve mode.",
                          mem_check->tname, mem_check->user,
                          mem_check->host, mem_check->host);
	continue;
      }
    }

    if (! mem_check->ok())
      delete mem_check;
    else if (my_hash_insert(&column_priv_hash,(byte*) mem_check))
    {
      delete mem_check;
      grant_option= FALSE;
      goto end_unlock;
    }
  }
  while (!t_table->file->index_next(t_table->record[0]));

  return_val=0;					// Return ok

end_unlock:
  t_table->file->ha_index_end();
  mysql_unlock_tables(thd, lock);
  thd->version--;				// Force close to free memory

end:
  close_thread_tables(thd);
  delete thd;
  if (org_thd)
    org_thd->store_globals();
  else
  {
    /* Remember that we don't have a THD */
    my_pthread_setspecific_ptr(THR_THD,  0);
  }
  DBUG_RETURN(return_val);
}


/*
 Reload grant array (table and column privileges) if possible

  SYNOPSIS
    grant_reload()
    thd			Thread handler

  NOTES
    Locked tables are checked by acl_init and doesn't have to be checked here
*/

void grant_reload(THD *thd)
{
  HASH old_column_priv_hash;
  bool old_grant_option;
  MEM_ROOT old_mem;
  DBUG_ENTER("grant_reload");

  rw_wrlock(&LOCK_grant);
  grant_version++;
  old_column_priv_hash= column_priv_hash;
  old_grant_option= grant_option;
  old_mem= memex;

  if (grant_init(thd))
  {						// Error. Revert to old hash
    DBUG_PRINT("error",("Reverting to old privileges"));
    grant_free();				/* purecov: deadcode */
    column_priv_hash= old_column_priv_hash;	/* purecov: deadcode */
    grant_option= old_grant_option;		/* purecov: deadcode */
    memex= old_mem;				/* purecov: deadcode */
  }
  else
  {
    hash_free(&old_column_priv_hash);
    free_root(&old_mem,MYF(0));
  }
  rw_unlock(&LOCK_grant);
  DBUG_VOID_RETURN;
}


/****************************************************************************
  Check table level grants

  SYNOPSIS
   bool check_grant()
   thd		Thread handler
   want_access  Bits of privileges user needs to have
   tables	List of tables to check. The user should have 'want_access'
		to all tables in list.
   show_table	<> 0 if we are in show table. In this case it's enough to have
	        any privilege for the table
   number	Check at most this number of tables.
   no_errors	If 0 then we write an error. The error is sent directly to
		the client

   RETURN
     0  ok
     1  Error: User did not have the requested privileges
****************************************************************************/

bool check_grant(THD *thd, ulong want_access, TABLE_LIST *tables,
		 uint show_table, uint number, bool no_errors)
{
  TABLE_LIST *table;
  char *user = thd->priv_user;
  DBUG_ENTER("check_grant");
  DBUG_ASSERT(number > 0);

  want_access&= ~thd->master_access;
  if (!want_access)
    DBUG_RETURN(0);                             // ok

  rw_rdlock(&LOCK_grant);
  for (table= tables; table && number--; table= table->next_global)
  {
    GRANT_TABLE *grant_table;
    if (!(~table->grant.privilege & want_access) || 
        table->derived || table->schema_table)
    {
      /*
        It is subquery in the FROM clause. VIEW set table->derived after
        table opening, but this function always called before table opening.
      */
      table->grant.want_privilege= 0;
      continue;					// Already checked
    }
    if (!(grant_table= table_hash_search(thd->host,thd->ip,
                                         table->db,user, table->real_name,0)))
    {
      want_access &= ~table->grant.privilege;
      goto err;					// No grants
    }
    if (show_table)
      continue;					// We have some priv on this

    table->grant.grant_table=grant_table;	// Remember for column test
    table->grant.version=grant_version;
    table->grant.privilege|= grant_table->privs;
    table->grant.want_privilege= ((want_access & COL_ACLS)
				  & ~table->grant.privilege);

    if (!(~table->grant.privilege & want_access))
      continue;

    if (want_access & ~(grant_table->cols | table->grant.privilege))
    {
      want_access &= ~(grant_table->cols | table->grant.privilege);
      goto err;					// impossible
    }
  }
  rw_unlock(&LOCK_grant);
  DBUG_RETURN(0);

err:
  rw_unlock(&LOCK_grant);
  if (!no_errors)				// Not a silent skip of table
  {
    const char *command="";
    if (want_access & SELECT_ACL)
      command= "select";
    else if (want_access & INSERT_ACL)
      command= "insert";
    else if (want_access & UPDATE_ACL)
      command= "update";
    else if (want_access & DELETE_ACL)
      command= "delete";
    else if (want_access & DROP_ACL)
      command= "drop";
    else if (want_access & CREATE_ACL)
      command= "create";
    else if (want_access & ALTER_ACL)
      command= "alter";
    else if (want_access & INDEX_ACL)
      command= "index";
    else if (want_access & GRANT_ACL)
      command= "grant";
    else if (want_access & CREATE_VIEW_ACL)
      command= "create view";
    else if (want_access & SHOW_VIEW_ACL)
      command= "show create view";
    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
             command,
             thd->priv_user,
             thd->host_or_ip,
             table ? table->real_name : "unknown");
  }
  DBUG_RETURN(1);
}


bool check_grant_column(THD *thd, GRANT_INFO *grant,
			char*db_name, char *table_name,
			const char *name, uint length, uint show_tables)
{
  GRANT_TABLE *grant_table;
  GRANT_COLUMN *grant_column;

  ulong want_access= grant->want_privilege & ~grant->privilege;
  if (!want_access)
    return 0;					// Already checked

  rw_rdlock(&LOCK_grant);

  /* reload table if someone has modified any grants */

  if (grant->version != grant_version)
  {
    grant->grant_table=
      table_hash_search(thd->host, thd->ip, db_name,
			thd->priv_user,
			table_name, 0);	/* purecov: inspected */
    grant->version= grant_version;		/* purecov: inspected */
  }
  if (!(grant_table= grant->grant_table))
    goto err;					/* purecov: deadcode */

  grant_column=column_hash_search(grant_table, name, length);
  if (grant_column && !(~grant_column->rights & want_access))
  {
    rw_unlock(&LOCK_grant);
    return 0;
  }
#ifdef NOT_USED
  if (show_tables && (grant_column || grant->privilege & COL_ACLS))
  {
    rw_unlock(&LOCK_grant);			/* purecov: deadcode */
    return 0;					/* purecov: deadcode */
  }
#endif

err:
  rw_unlock(&LOCK_grant);
  if (!show_tables)
  {
    char command[128];
    get_privilege_desc(command, sizeof(command), want_access);
    my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
             command,
             thd->priv_user,
             thd->host_or_ip,
             name,
             table_name);
  }
  return 1;
}


bool check_grant_all_columns(THD *thd, ulong want_access, GRANT_INFO *grant,
                             char* db_name, char *table_name,
                             Field_iterator *fields)
{
  GRANT_TABLE *grant_table;
  GRANT_COLUMN *grant_column;
  Field *field=0;

  want_access &= ~grant->privilege;
  if (!want_access)
    return 0;				// Already checked
  if (!grant_option)
    goto err2;

  rw_rdlock(&LOCK_grant);

  /* reload table if someone has modified any grants */

  if (grant->version != grant_version)
  {
    grant->grant_table=
      table_hash_search(thd->host, thd->ip, db_name,
			thd->priv_user,
			table_name, 0);	/* purecov: inspected */
    grant->version= grant_version;		/* purecov: inspected */
  }
  /* The following should always be true */
  if (!(grant_table= grant->grant_table))
    goto err;					/* purecov: inspected */

  for (; !fields->end_of_fields(); fields->next())
  {
    const char *field_name= fields->name();
    grant_column= column_hash_search(grant_table, field_name,
				    (uint) strlen(field_name));
    if (!grant_column || (~grant_column->rights & want_access))
      goto err;
  }
  rw_unlock(&LOCK_grant);
  return 0;

err:
  rw_unlock(&LOCK_grant);
err2:
  const char *command= "";
  if (want_access & SELECT_ACL)
    command= "select";
  else if (want_access & INSERT_ACL)
    command= "insert";
  my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
           command,
           thd->priv_user,
           thd->host_or_ip,
           fields->name(),
           table_name);
  return 1;
}


/*
  Check if a user has the right to access a database
  Access is accepted if he has a grant for any table in the database
  Return 1 if access is denied
*/

bool check_grant_db(THD *thd,const char *db)
{
  char helping [NAME_LEN+USERNAME_LENGTH+2];
  uint len;
  bool error=1;

  len  = (uint) (strmov(strmov(helping,thd->priv_user)+1,db)-helping)+ 1;
  rw_rdlock(&LOCK_grant);

  for (uint idx=0 ; idx < column_priv_hash.records ; idx++)
  {
    GRANT_TABLE *grant_table= (GRANT_TABLE*) hash_element(&column_priv_hash,
							  idx);
    if (len < grant_table->key_length &&
	!memcmp(grant_table->hash_key,helping,len) &&
	(thd->host && !wild_case_compare(system_charset_info,
                                         thd->host,grant_table->host) ||
	 (thd->ip && !wild_case_compare(system_charset_info,
                                        thd->ip,grant_table->host))))
    {
      error=0;					// Found match
      break;
    }
  }
  rw_unlock(&LOCK_grant);
  return error;
}

/*****************************************************************************
  Functions to retrieve the grant for a table/column  (for SHOW functions)
*****************************************************************************/

ulong get_table_grant(THD *thd, TABLE_LIST *table)
{
  ulong privilege;
  char *user = thd->priv_user;
  const char *db = table->db ? table->db : thd->db;
  GRANT_TABLE *grant_table;

  rw_rdlock(&LOCK_grant);
#ifdef EMBEDDED_LIBRARY
  grant_table= NULL;
#else
  grant_table= table_hash_search(thd->host, thd->ip, db, user,
				 table->real_name, 0);
#endif
  table->grant.grant_table=grant_table; // Remember for column test
  table->grant.version=grant_version;
  if (grant_table)
    table->grant.privilege|= grant_table->privs;
  privilege= table->grant.privilege;
  rw_unlock(&LOCK_grant);
  return privilege;
}


ulong get_column_grant(THD *thd, GRANT_INFO *grant,
                       const char *db_name, const char *table_name,
                       const char *field_name)
{
  GRANT_TABLE *grant_table;
  GRANT_COLUMN *grant_column;
  ulong priv;

  rw_rdlock(&LOCK_grant);
  /* reload table if someone has modified any grants */
  if (grant->version != grant_version)
  {
    grant->grant_table=
      table_hash_search(thd->host, thd->ip, db_name,
			thd->priv_user,
			table_name, 0);	        /* purecov: inspected */
    grant->version= grant_version;              /* purecov: inspected */
  }

  if (!(grant_table= grant->grant_table))
    priv= grant->privilege;
  else
  {
    grant_column= column_hash_search(grant_table, field_name,
                                     (uint) strlen(field_name));
    if (!grant_column)
      priv= grant->privilege;
    else
      priv= grant->privilege | grant_column->rights;
  }
  rw_unlock(&LOCK_grant);
  return priv;
}


/* Help function for mysql_show_grants */

static void add_user_option(String *grant, ulong value, const char *name)
{
  if (value)
  {
    char buff[22], *p; // just as in int2str
    grant->append(' ');
    grant->append(name, strlen(name));
    grant->append(' ');
    p=int10_to_str(value, buff, 10);
    grant->append(buff,p-buff);
  }
}

static const char *command_array[]=
{
  "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "RELOAD",
  "SHUTDOWN", "PROCESS","FILE", "GRANT", "REFERENCES", "INDEX",
  "ALTER", "SHOW DATABASES", "SUPER", "CREATE TEMPORARY TABLES",
  "LOCK TABLES", "EXECUTE", "REPLICATION SLAVE", "REPLICATION CLIENT",
  "CREATE VIEW", "SHOW VIEW"
};

static uint command_lengths[]=
{
  6, 6, 6, 6, 6, 4, 6, 8, 7, 4, 5, 10, 5, 5, 14, 5, 23, 11, 7, 17, 18, 11, 9
};


/*
  SHOW GRANTS;  Send grants for a user to the client

  IMPLEMENTATION
   Send to client grant-like strings depicting user@host privileges
*/

bool mysql_show_grants(THD *thd,LEX_USER *lex_user)
{
  ulong want_access;
  uint counter,index;
  int  error = 0;
  ACL_USER *acl_user;
  ACL_DB *acl_db;
  char buff[1024];
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysql_show_grants");

  LINT_INIT(acl_user);
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(TRUE);
  }

  if (!lex_user->host.str)
  {
    lex_user->host.str= (char*) "%";
    lex_user->host.length=1;
  }
  if (lex_user->host.length > HOSTNAME_LENGTH ||
      lex_user->user.length > USERNAME_LENGTH)
  {
    my_message(ER_GRANT_WRONG_HOST_OR_USER, ER(ER_GRANT_WRONG_HOST_OR_USER),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  for (counter=0 ; counter < acl_users.elements ; counter++)
  {
    const char *user,*host;
    acl_user=dynamic_element(&acl_users,counter,ACL_USER*);
    if (!(user=acl_user->user))
      user= "";
    if (!(host=acl_user->host.hostname))
      host= "";
    if (!strcmp(lex_user->user.str,user) &&
	!my_strcasecmp(system_charset_info, lex_user->host.str, host))
      break;
  }
  if (counter == acl_users.elements)
  {
    my_error(ER_NONEXISTING_GRANT, MYF(0),
             lex_user->user.str, lex_user->host.str);
    DBUG_RETURN(TRUE);
  }

  Item_string *field=new Item_string("",0,&my_charset_latin1);
  List<Item> field_list;
  field->name=buff;
  field->max_length=1024;
  strxmov(buff,"Grants for ",lex_user->user.str,"@",
	  lex_user->host.str,NullS);
  field_list.push_back(field);
  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));

  /* Add first global access grants */
  {
    String global(buff,sizeof(buff),system_charset_info);
    global.length(0);
    global.append("GRANT ",6);

    want_access= acl_user->access;
    if (test_all_bits(want_access, (GLOBAL_ACLS & ~ GRANT_ACL)))
      global.append("ALL PRIVILEGES",14);
    else if (!(want_access & ~GRANT_ACL))
      global.append("USAGE",5);
    else
    {
      bool found=0;
      ulong j,test_access= want_access & ~GRANT_ACL;
      for (counter=0, j = SELECT_ACL;j <= GLOBAL_ACLS;counter++,j <<= 1)
      {
	if (test_access & j)
	{
	  if (found)
	    global.append(", ",2);
	  found=1;
	  global.append(command_array[counter],command_lengths[counter]);
	}
      }
    }
    global.append (" ON *.* TO '",12);
    global.append(lex_user->user.str, lex_user->user.length,
		  system_charset_info);
    global.append ("'@'",3);
    global.append(lex_user->host.str,lex_user->host.length,
		  system_charset_info);
    global.append ('\'');
    if (acl_user->salt_len)
    {
      char passwd_buff[SCRAMBLED_PASSWORD_CHAR_LENGTH+1];
      if (acl_user->salt_len == SCRAMBLE_LENGTH)
        make_password_from_salt(passwd_buff, acl_user->salt);
      else
        make_password_from_salt_323(passwd_buff, (ulong *) acl_user->salt);
      global.append(" IDENTIFIED BY PASSWORD '",25);
      global.append(passwd_buff);
      global.append('\'');
    }
    /* "show grants" SSL related stuff */
    if (acl_user->ssl_type == SSL_TYPE_ANY)
      global.append(" REQUIRE SSL",12);
    else if (acl_user->ssl_type == SSL_TYPE_X509)
      global.append(" REQUIRE X509",13);
    else if (acl_user->ssl_type == SSL_TYPE_SPECIFIED)
    {
      int ssl_options = 0;
      global.append(" REQUIRE ",9);
      if (acl_user->x509_issuer)
      {
	ssl_options++;
	global.append("ISSUER \'",8);
	global.append(acl_user->x509_issuer,strlen(acl_user->x509_issuer));
	global.append('\'');
      }
      if (acl_user->x509_subject)
      {
	if (ssl_options++)
	  global.append(' ');
	global.append("SUBJECT \'",9);
	global.append(acl_user->x509_subject,strlen(acl_user->x509_subject),
                      system_charset_info);
	global.append('\'');
      }
      if (acl_user->ssl_cipher)
      {
	if (ssl_options++)
	  global.append(' ');
	global.append("CIPHER '",8);
	global.append(acl_user->ssl_cipher,strlen(acl_user->ssl_cipher),
                      system_charset_info);
	global.append('\'');
      }
    }
    if ((want_access & GRANT_ACL) ||
	(acl_user->user_resource.questions | acl_user->user_resource.updates |
	 acl_user->user_resource.connections))
    {
      global.append(" WITH",5);
      if (want_access & GRANT_ACL)
	global.append(" GRANT OPTION",13);
      add_user_option(&global, acl_user->user_resource.questions,
		      "MAX_QUERIES_PER_HOUR");
      add_user_option(&global, acl_user->user_resource.updates,
		      "MAX_UPDATES_PER_HOUR");
      add_user_option(&global, acl_user->user_resource.connections,
		      "MAX_CONNECTIONS_PER_HOUR");
    }
    protocol->prepare_for_resend();
    protocol->store(global.ptr(),global.length(),global.charset());
    if (protocol->write())
    {
      error= -1;
      goto end;
    }
  }

  /* Add database access */
  for (counter=0 ; counter < acl_dbs.elements ; counter++)
  {
    const char *user, *host;

    acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);
    if (!(user=acl_db->user))
      user= "";
    if (!(host=acl_db->host.hostname))
      host= "";

    if (!strcmp(lex_user->user.str,user) &&
	!my_strcasecmp(system_charset_info, lex_user->host.str, host))
    {
      want_access=acl_db->access;
      if (want_access)
      {
	String db(buff,sizeof(buff),system_charset_info);
	db.length(0);
	db.append("GRANT ",6);

	if (test_all_bits(want_access,(DB_ACLS & ~GRANT_ACL)))
	  db.append("ALL PRIVILEGES",14);
	else if (!(want_access & ~GRANT_ACL))
	  db.append("USAGE",5);
	else
	{
	  int found=0, cnt;
	  ulong j,test_access= want_access & ~GRANT_ACL;
	  for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
	  {
	    if (test_access & j)
	    {
	      if (found)
		db.append(", ",2);
	      found = 1;
	      db.append(command_array[cnt],command_lengths[cnt]);
	    }
	  }
	}
	db.append (" ON ",4);
	append_identifier(thd, &db, acl_db->db, strlen(acl_db->db));
	db.append (".* TO '",7);
	db.append(lex_user->user.str, lex_user->user.length,
		  system_charset_info);
	db.append ("'@'",3);
	db.append(lex_user->host.str, lex_user->host.length,
                  system_charset_info);
	db.append ('\'');
	if (want_access & GRANT_ACL)
	  db.append(" WITH GRANT OPTION",18);
	protocol->prepare_for_resend();
	protocol->store(db.ptr(),db.length(),db.charset());
	if (protocol->write())
	{
	  error= -1;
	  goto end;
	}
      }
    }
  }

  /* Add table & column access */
  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user;
    GRANT_TABLE *grant_table= (GRANT_TABLE*) hash_element(&column_priv_hash,
							  index);

    if (!(user=grant_table->user))
      user= "";

    if (!strcmp(lex_user->user.str,user) &&
	!my_strcasecmp(system_charset_info, lex_user->host.str,
                       grant_table->orig_host))
    {
      ulong table_access= grant_table->privs;
      if ((table_access | grant_table->cols) != 0)
      {
	String global(buff, sizeof(buff), system_charset_info);
	ulong test_access= (table_access | grant_table->cols) & ~GRANT_ACL;

	global.length(0);
	global.append("GRANT ",6);

	if (test_all_bits(table_access, (TABLE_ACLS & ~GRANT_ACL)))
	  global.append("ALL PRIVILEGES",14);
	else if (!test_access)
 	  global.append("USAGE",5);
	else
	{
          /* Add specific column access */
	  int found= 0;
	  ulong j;

	  for (counter= 0, j= SELECT_ACL; j <= TABLE_ACLS; counter++, j<<= 1)
	  {
	    if (test_access & j)
	    {
	      if (found)
		global.append(", ",2);
	      found= 1;
	      global.append(command_array[counter],command_lengths[counter]);

	      if (grant_table->cols)
	      {
		uint found_col= 0;
		for (uint col_index=0 ;
		     col_index < grant_table->hash_columns.records ;
		     col_index++)
		{
		  GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
		    hash_element(&grant_table->hash_columns,col_index);
		  if (grant_column->rights & j)
		  {
		    if (!found_col)
		    {
		      found_col= 1;
		      /*
			If we have a duplicated table level privilege, we
			must write the access privilege name again.
		      */
		      if (table_access & j)
		      {
			global.append(", ", 2);
			global.append(command_array[counter],
				      command_lengths[counter]);
		      }
		      global.append(" (",2);
		    }
		    else
		      global.append(", ",2);
		    global.append(grant_column->column,
				  grant_column->key_length,
				  system_charset_info);
		  }
		}
		if (found_col)
		  global.append(')');
	      }
	    }
	  }
	}
	global.append(" ON ",4);
	append_identifier(thd, &global, grant_table->db,
			  strlen(grant_table->db));
	global.append('.');
	append_identifier(thd, &global, grant_table->tname,
			  strlen(grant_table->tname));
	global.append(" TO '",5);
	global.append(lex_user->user.str, lex_user->user.length,
		      system_charset_info);
	global.append("'@'",3);
	global.append(lex_user->host.str,lex_user->host.length,
		      system_charset_info);
	global.append('\'');
	if (table_access & GRANT_ACL)
	  global.append(" WITH GRANT OPTION",18);
	protocol->prepare_for_resend();
	protocol->store(global.ptr(),global.length(),global.charset());
	if (protocol->write())
	{
	  error= -1;
	  break;
	}
      }
    }
  }
end:
  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);

  send_eof(thd);
  DBUG_RETURN(error);
}


/*
  Make a clear-text version of the requested privilege.
*/

void get_privilege_desc(char *to, uint max_length, ulong access)
{
  uint pos;
  char *start=to;
  DBUG_ASSERT(max_length >= 30);		// For end ',' removal

  if (access)
  {
    max_length--;				// Reserve place for end-zero
    for (pos=0 ; access ; pos++, access>>=1)
    {
      if ((access & 1) &&
	  command_lengths[pos] + (uint) (to-start) < max_length)
      {
	to= strmov(to, command_array[pos]);
	*to++=',';
      }
    }
    to--;					// Remove end ','
  }
  *to=0;
}


void get_mqh(const char *user, const char *host, USER_CONN *uc)
{
  ACL_USER *acl_user;
  if (initialized && (acl_user= find_acl_user(host,user)))
    uc->user_resources= acl_user->user_resource;
  else
    bzero((char*) &uc->user_resources, sizeof(uc->user_resources));
}

/*
  Open the grant tables.

  SYNOPSIS
    open_grant_tables()
    thd                         The current thread.
    tables (out)                The 4 elements array for the opened tables.

  DESCRIPTION
    Tables are numbered as follows:
    0 user
    1 db
    2 tables_priv
    3 columns_priv

  RETURN
    1           Skip GRANT handling during replication.
    0           OK.
    < 0         Error.
*/

int open_grant_tables(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("open_grant_tables");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(-1);
  }

  bzero((char*) tables, 4*sizeof(*tables));
  tables->alias= tables->real_name= (char*) "user";
  (tables+1)->alias= (tables+1)->real_name= (char*) "db";
  (tables+2)->alias= (tables+2)->real_name= (char*) "tables_priv";
  (tables+3)->alias= (tables+3)->real_name= (char*) "columns_priv";
  tables->next_local= tables->next_global= tables+1;
  (tables+1)->next_local= (tables+1)->next_global= tables+2;
  (tables+2)->next_local= (tables+2)->next_global= tables+3;
  tables->lock_type= (tables+1)->lock_type=
    (tables+2)->lock_type= (tables+3)->lock_type= TL_WRITE;
  tables->db= (tables+1)->db= (tables+2)->db= (tables+3)->db=(char*) "mysql";

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && table_rules_on)
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating=tables[1].updating=tables[2].updating=tables[3].updating=1;
    if (!tables_ok(0, tables))
      DBUG_RETURN(1);
    tables[0].updating=tables[1].updating=tables[2].updating=tables[3].updating=0;
  }
#endif

  if (simple_open_n_lock_tables(thd, tables))
  {						// This should never happen
    close_thread_tables(thd);
    DBUG_RETURN(-1);
  }

  DBUG_RETURN(0);
}

ACL_USER *check_acl_user(LEX_USER *user_name,
			 uint *acl_acl_userdx)
{
  ACL_USER *acl_user= 0;
  uint counter;

  for (counter= 0 ; counter < acl_users.elements ; counter++)
  {
    const char *user,*host;
    acl_user= dynamic_element(&acl_users, counter, ACL_USER*);
    if (!(user=acl_user->user))
      user= "";
    if (!(host=acl_user->host.hostname))
      host= "%";
    if (!strcmp(user_name->user.str,user) &&
	!my_strcasecmp(system_charset_info, user_name->host.str, host))
      break;
  }
  if (counter == acl_users.elements)
    return 0;

  *acl_acl_userdx= counter;
  return acl_user;
}


/*
  Modify a privilege table.

  SYNOPSIS
    modify_grant_table()
    table                       The table to modify.
    host_field                  The host name field.
    user_field                  The user name field.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
  Update user/host in the current record if user_to is not NULL.
  Delete the current record if user_to is NULL.

  RETURN
    0           OK.
    != 0        Error.
*/

static int modify_grant_table(TABLE *table, Field *host_field,
                              Field *user_field, LEX_USER *user_to)
{
  int error;
  DBUG_ENTER("modify_grant_table");

  if (user_to)
  {
    /* rename */
    store_record(table, record[1]);
    host_field->store(user_to->host.str, user_to->host.length,
                      system_charset_info);
    user_field->store(user_to->user.str, user_to->user.length,
                      system_charset_info);
    if ((error= table->file->update_row(table->record[1], table->record[0])))
      table->file->print_error(error, MYF(0));
  }
  else
  {
    /* delete */
    if ((error=table->file->delete_row(table->record[0])))
      table->file->print_error(error, MYF(0));
  }

  DBUG_RETURN(error);
}


/*
  Handle a privilege table.

  SYNOPSIS
    handle_grant_table()
    tables                      The array with the four open tables.
    table_no                    The number of the table to handle (0..3).
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Scan through all records in a grant table and apply the requested
    operation. For the "user" table, a single index access is sufficient,
    since there is an unique index on (host, user).
    Delete from grant table if drop is true.
    Update in grant table if drop is false and user_to is not NULL.
    Search in grant table if drop is false and user_to is NULL.
    Tables are numbered as follows:
    0 user
    1 db
    2 tables_priv
    3 columns_priv

  RETURN
    > 0         At least one record matched.
    0           OK, but no record matched.
    < 0         Error.
*/

static int handle_grant_table(TABLE_LIST *tables, uint table_no, bool drop,
                              LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int error;
  TABLE *table= tables[table_no].table;
  Field *host_field= table->field[0];
  Field *user_field= table->field[table_no ? 2 : 1];
  char *host_str= user_from->host.str;
  char *user_str= user_from->user.str;
  const char *host;
  const char *user;
  DBUG_ENTER("handle_grant_table");

  if (! table_no) // mysql.user table
  {
    /*
      The 'user' table has an unique index on (host, user).
      Thus, we can handle everything with a single index access.
      The host- and user fields are consecutive in the user table records.
      So we set host- and user fields of table->record[0] and use the
      pointer to the host field as key.
      index_read_idx() will replace table->record[0] (its first argument)
      by the searched record, if it exists.
    */
    DBUG_PRINT("info",("read table: '%s'  search: '%s'@'%s'",
                       table->real_name, user_str, host_str));
    host_field->store(host_str, user_from->host.length, system_charset_info);
    user_field->store(user_str, user_from->user.length, system_charset_info);
    if ((error= table->file->index_read_idx(table->record[0], 0,
                                            (byte*) host_field->ptr, 0,
                                            HA_READ_KEY_EXACT)))
    {
      if (error != HA_ERR_KEY_NOT_FOUND)
      {
        table->file->print_error(error, MYF(0));
        result= -1;
      }
    }
    else
    {
      /* If requested, delete or update the record. */
      result= ((drop || user_to) &&
               modify_grant_table(table, host_field, user_field, user_to)) ?
        -1 : 1; /* Error or found. */
    }
    DBUG_PRINT("info",("read result: %d", result));
  }
  else
  {
    /*
      The non-'user' table do not have indexes on (host, user).
      And their host- and user fields are not consecutive.
      Thus, we need to do a table scan to find all matching records.
    */
    if ((error= table->file->ha_rnd_init(1)))
    {
      table->file->print_error(error, MYF(0));
      result= -1;
    }
    else
    {
#ifdef EXTRA_DEBUG
      DBUG_PRINT("info",("scan table: '%s'  search: '%s'@'%s'",
                         table->real_name, user_str, host_str));
#endif
      while ((error= table->file->rnd_next(table->record[0])) != 
             HA_ERR_END_OF_FILE)
      {
        if (error)
        {
          /* Most probable 'deleted record'. */
          DBUG_PRINT("info",("scan error: %d", error));
          continue;
        }
        if (! (host= get_field(&mem, host_field)))
          host= "";
        if (! (user= get_field(&mem, user_field)))
          user= "";

#ifdef EXTRA_DEBUG
        DBUG_PRINT("loop",("scan fields: '%s'@'%s' '%s' '%s' '%s'",
                           user, host,
                           get_field(&mem, table->field[1]) /*db*/,
                           get_field(&mem, table->field[3]) /*table*/,
                           get_field(&mem, table->field[4]) /*column*/));
#endif
        if (strcmp(user_str, user) ||
            my_strcasecmp(system_charset_info, host_str, host))
          continue;

        /* If requested, delete or update the record. */
        result= ((drop || user_to) &&
                 modify_grant_table(table, host_field, user_field, user_to)) ?
          -1 : result ? result : 1; /* Error or keep result or found. */
        /* If search is requested, we do not need to search further. */
        if (! drop && ! user_to)
          break ;
      }
      (void) table->file->ha_rnd_end();
      DBUG_PRINT("info",("scan result: %d", result));
    }
  }

  DBUG_RETURN(result);
}


/*
  Handle an in-memory privilege structure.

  SYNOPSIS
    handle_grant_struct()
    struct_no                   The number of the structure to handle (0..2).
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Scan through all elements in an in-memory grant structure and apply
    the requested operation.
    Delete from grant structure if drop is true.
    Update in grant structure if drop is false and user_to is not NULL.
    Search in grant structure if drop is false and user_to is NULL.
    Structures are numbered as follows:
    0 acl_users
    1 acl_dbs
    2 column_priv_hash

  RETURN
    > 0         At least one element matched.
    0           OK, but no element matched.
*/

static int handle_grant_struct(uint struct_no, bool drop,
                               LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  uint idx;
  uint elements;
  const char *user;
  const char *host;
  ACL_USER *acl_user;
  ACL_DB *acl_db;
  GRANT_TABLE *grant_table;
  DBUG_ENTER("handle_grant_struct");
  LINT_INIT(acl_user);
  LINT_INIT(acl_db);
  LINT_INIT(grant_table);
  DBUG_PRINT("info",("scan struct: %u  search: '%s'@'%s'",
                     struct_no, user_from->user.str, user_from->host.str));

  /* Get the number of elements in the in-memory structure. */
  switch (struct_no)
  {
  case 0:
    elements= acl_users.elements;
    break;
  case 1:
    elements= acl_dbs.elements;
    break;
  default:
    elements= column_priv_hash.records;
  }

#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  search    user: '%s'  host: '%s'",
                       struct_no, user_from->user.str, user_from->host.str));
#endif
  /* Loop over all elements. */
  for (idx= 0; idx < elements; idx++)
  {
    /*
      Get a pointer to the element.
      Unfortunaltely, the host default differs for the structures.
    */
    switch (struct_no)
    {
    case 0:
      acl_user= dynamic_element(&acl_users, idx, ACL_USER*);
      user= acl_user->user;
      if (!(host= acl_user->host.hostname))
        host= "%";
      break;

    case 1:
      acl_db= dynamic_element(&acl_dbs, idx, ACL_DB*);
      user= acl_db->user;
      host= acl_db->host.hostname;
      break;

    default:
      grant_table= (GRANT_TABLE*) hash_element(&column_priv_hash, idx);
      user= grant_table->user;
      host= grant_table->host;
    }
    if (! user)
        user= "";
    if (! host)
        host= "";
#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  index: %u  user: '%s'  host: '%s'",
                       struct_no, idx, user, host));
#endif
    if (strcmp(user_from->user.str, user) ||
        my_strcasecmp(system_charset_info, user_from->host.str, host))
      continue;

    result= 1; /* At least one element found. */
    if ( drop )
    {
      switch ( struct_no )
      {
      case 0:
        delete_dynamic_element(&acl_users, idx);
        break;

      case 1:
        delete_dynamic_element(&acl_dbs, idx);
        break;

      default:
        hash_delete(&column_priv_hash, (byte*) grant_table);
      }
      elements--;
      idx--;
    }
    else if ( user_to )
    {
      switch ( struct_no )
      {
      case 0:
        acl_user->user= strdup_root(&mem, user_to->user.str);
        acl_user->host.hostname= strdup_root(&mem, user_to->host.str);
        break;

      case 1:
        acl_db->user= strdup_root(&mem, user_to->user.str);
        acl_db->host.hostname= strdup_root(&mem, user_to->host.str);
        break;

      default:
        grant_table->user= strdup_root(&mem, user_to->user.str);
        grant_table->host= strdup_root(&mem, user_to->host.str);
      }
    }
    else
    {
      /* If search is requested, we do not need to search further. */
      break;
    }
  }
#ifdef EXTRA_DEBUG
  DBUG_PRINT("loop",("scan struct: %u  result %d", struct_no, result));
#endif

  DBUG_RETURN(result);
}


/*
  Handle all privilege tables and in-memory privilege structures.

  SYNOPSIS
    handle_grant_data()
    tables                      The array with the four open tables.
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Go through all grant tables and in-memory grant structures and apply
    the requested operation.
    Delete from grant data if drop is true.
    Update in grant data if drop is false and user_to is not NULL.
    Search in grant data if drop is false and user_to is NULL.

  RETURN
    > 0         At least one element matched.
    0           OK, but no element matched.
    < 0         Error.
*/

static int handle_grant_data(TABLE_LIST *tables, bool drop,
                             LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int found;
  DBUG_ENTER("handle_grant_data");

  /* Handle user table. */
  if ((found= handle_grant_table(tables, 0, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle user array. */
    if ((handle_grant_struct(0, drop, user_from, user_to) && ! result) || found)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
  }

  /* Handle db table. */
  if ((found= handle_grant_table(tables, 1, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle db array. */
    if (((handle_grant_struct(1, drop, user_from, user_to) && ! result) ||
         found) && ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
  }

  /* Handle tables table. */
  if ((found= handle_grant_table(tables, 2, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch columns and in-memory array. */
    result= -1;
  }
  else
  {
    if (found && ! result)
    {
      result= 1; /* At least one record found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }

    /* Handle columns table. */
    if ((found= handle_grant_table(tables, 3, drop, user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle columns hash. */
      if (((handle_grant_struct(2, drop, user_from, user_to) && ! result) ||
           found) && ! result)
        result= 1; /* At least one record/element found. */
    }
  }
 end:
  DBUG_RETURN(result);
}

static void append_user(String *str, LEX_USER *user)
{
  if (str->length())
    str->append(',');
  str->append('\'');
  str->append(user->user.str);
  str->append("'@'");
  str->append(user->host.str);
  str->append('\'');
}

/*
  Create a list of users.

  SYNOPSIS
    mysql_create_user()
    thd                         The current thread.
    list                        The users to create.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_create_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  int found;
  String wrong_users;
  ulong sql_mode;
  LEX_USER *user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[4];
  DBUG_ENTER("mysql_create_user");

  /* CREATE USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables)))
    DBUG_RETURN(result != 1);

  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));

  while ((user_name= user_list++))
  {
    /*
      Search all in-memory structures and grant tables
      for a mention of the new user name.
    */
    if ((found= handle_grant_data(tables, 0, user_name, NULL)))
    {
      append_user(&wrong_users, user_name);
      result= TRUE;
      continue;
    }

    sql_mode= thd->variables.sql_mode;
    thd->variables.sql_mode&= ~MODE_NO_AUTO_CREATE_USER;
    if (replace_user_table(thd, tables[0].table, *user_name, 0, 0, 1))
    {
      append_user(&wrong_users, user_name);
      result= TRUE;
    }
    thd->variables.sql_mode= sql_mode;
  }

  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);
  close_thread_tables(thd);
  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "CREATE USER", wrong_users.c_ptr());
  DBUG_RETURN(result);
}


/*
  Drop a list of users and all their privileges.

  SYNOPSIS
    mysql_drop_user()
    thd                         The current thread.
    list                        The users to drop.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_drop_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  int found;
  String wrong_users;
  LEX_USER *user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[4];
  DBUG_ENTER("mysql_drop_user");

  /* DROP USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables)))
    DBUG_RETURN(result != 1);

  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));

  while ((user_name= user_list++))
  {
    if ((found= handle_grant_data(tables, 1, user_name, NULL)) <= 0)
    {
      append_user(&wrong_users, user_name);
      result= TRUE;
    }
  }

  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);
  close_thread_tables(thd);
  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "DROP USER", wrong_users.c_ptr());
  DBUG_RETURN(result);
}


/*
  Rename a user.

  SYNOPSIS
    mysql_rename_user()
    thd                         The current thread.
    list                        The user name pairs: (from, to).

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_rename_user(THD *thd, List <LEX_USER> &list)
{
  int result= 0;
  int found;
  String wrong_users;
  LEX_USER *user_from;
  LEX_USER *user_to;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[4];
  DBUG_ENTER("mysql_rename_user");

  /* RENAME USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables)))
    DBUG_RETURN(result != 1);

  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));

  while ((user_from= user_list++))
  {
    user_to= user_list++;
    DBUG_ASSERT(user_to); /* Syntax enforces pairs of users. */

    /*
      Search all in-memory structures and grant tables
      for a mention of the new user name.
    */
    if (handle_grant_data(tables, 0, user_to, NULL) ||
        handle_grant_data(tables, 0, user_from, user_to) <= 0)
    {
      append_user(&wrong_users, user_from);
      result= TRUE;
    }
  }

  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);
  close_thread_tables(thd);
  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "RENAME USER", wrong_users.c_ptr());
  DBUG_RETURN(result);
}

/*
  Revoke all privileges from a list of users.

  SYNOPSIS
    mysql_revoke_all()
    thd                         The current thread.
    list                        The users to revoke all privileges from.

  RETURN
    > 0         Error. Error message already sent.
    0           OK.
    < 0         Error. Error message not yet sent.
*/

bool mysql_revoke_all(THD *thd,  List <LEX_USER> &list)
{
  uint counter, revoked;
  int result;
  ACL_DB *acl_db;
  TABLE_LIST tables[4];
  DBUG_ENTER("mysql_revoke_all");

  if ((result= open_grant_tables(thd, tables)))
    DBUG_RETURN(result != 1);

  rw_wrlock(&LOCK_grant);
  VOID(pthread_mutex_lock(&acl_cache->lock));

  LEX_USER *lex_user;
  List_iterator <LEX_USER> user_list(list);
  while ((lex_user=user_list++))
  {
    if (!check_acl_user(lex_user, &counter))
    {
      sql_print_error("REVOKE ALL PRIVILEGES, GRANT: User '%s'@'%s' not exists",
		      lex_user->user.str,
		      lex_user->host.str);
      result= -1;
      continue;
    }

    if (replace_user_table(thd, tables[0].table,
			   *lex_user, ~0, 1, 0))
    {
      result= -1;
      continue;
    }

    /* Remove db access privileges */
    /*
      Because acl_dbs and column_priv_hash shrink and may re-order
      as privileges are removed, removal occurs in a repeated loop
      until no more privileges are revoked.
     */
    do
    {
      for (counter= 0, revoked= 0 ; counter < acl_dbs.elements ; )
      {
	const char *user,*host;

	acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);
	if (!(user=acl_db->user))
	  user= "";
	if (!(host=acl_db->host.hostname))
	  host= "";

	if (!strcmp(lex_user->user.str,user) &&
	    !my_strcasecmp(system_charset_info, lex_user->host.str, host))
	{
	  if (!replace_db_table(tables[1].table, acl_db->db, *lex_user, ~0, 1))
	  {
	    /*
	      Don't increment counter as replace_db_table deleted the
	      current element in acl_dbs.
	     */
	    revoked= 1;
	    continue;
	  }
	  result= -1; // Something went wrong
	}
	counter++;
      }
    } while (revoked);

    /* Remove column access */
    do
    {
      for (counter= 0, revoked= 0 ; counter < column_priv_hash.records ; )
      {
	const char *user,*host;
	GRANT_TABLE *grant_table= (GRANT_TABLE*)hash_element(&column_priv_hash,
							     counter);
	if (!(user=grant_table->user))
	  user= "";
	if (!(host=grant_table->host))
	  host= "";

	if (!strcmp(lex_user->user.str,user) &&
	    !my_strcasecmp(system_charset_info, lex_user->host.str, host))
	{
	  if (replace_table_table(thd,grant_table,tables[2].table,*lex_user,
				  grant_table->db,
				  grant_table->tname,
				  ~0, 0, 1))
	  {
	    result= -1;
	  }
	  else
	  {
	    if (!grant_table->cols)
	    {
	      revoked= 1;
	      continue;
	    }
	    List<LEX_COLUMN> columns;
	    if (!replace_column_table(grant_table,tables[3].table, *lex_user,
				      columns,
				      grant_table->db,
				      grant_table->tname,
				      ~0, 1))
	    {
	      revoked= 1;
	      continue;
	    }
	    result= -1;
	  }
	}
	counter++;
      }
    } while (revoked);
  }

  VOID(pthread_mutex_unlock(&acl_cache->lock));
  rw_unlock(&LOCK_grant);
  close_thread_tables(thd);

  if (result)
    my_message(ER_REVOKE_GRANTS, ER(ER_REVOKE_GRANTS), MYF(0));

  DBUG_RETURN(result);
}


/*****************************************************************************
  Instantiate used templates
*****************************************************************************/

#ifdef __GNUC__
template class List_iterator<LEX_COLUMN>;
template class List_iterator<LEX_USER>;
template class List<LEX_COLUMN>;
template class List<LEX_USER>;
#endif

#endif /*NO_EMBEDDED_ACCESS_CHECKS */


int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr)
{
  reg3 int flag;
  DBUG_ENTER("wild_case_compare");
  DBUG_PRINT("enter",("str: '%s'  wildstr: '%s'",str,wildstr));
  while (*wildstr)
  {
    while (*wildstr && *wildstr != wild_many && *wildstr != wild_one)
    {
      if (*wildstr == wild_prefix && wildstr[1])
	wildstr++;
      if (my_toupper(cs, *wildstr++) !=
          my_toupper(cs, *str++)) DBUG_RETURN(1);
    }
    if (! *wildstr ) DBUG_RETURN (*str != 0);
    if (*wildstr++ == wild_one)
    {
      if (! *str++) DBUG_RETURN (1);	/* One char; skip */
    }
    else
    {						/* Found '*' */
      if (!*wildstr) DBUG_RETURN(0);		/* '*' as last char: OK */
      flag=(*wildstr != wild_many && *wildstr != wild_one);
      do
      {
	if (flag)
	{
	  char cmp;
	  if ((cmp= *wildstr) == wild_prefix && wildstr[1])
	    cmp=wildstr[1];
	  cmp=my_toupper(cs, cmp);
	  while (*str && my_toupper(cs, *str) != cmp)
	    str++;
	  if (!*str) DBUG_RETURN (1);
	}
	if (wild_case_compare(cs, str,wildstr) == 0) DBUG_RETURN (0);
      } while (*str++);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN (*str != '\0');
}


void update_schema_privilege(TABLE *table, char *buff, const char* db,
                             const char* t_name, const char* column,
                             uint col_length, const char *priv, 
                             uint priv_length, const char* is_grantable)
{
  int i= 2;
  CHARSET_INFO *cs= system_charset_info;
  restore_record(table, default_values);
  table->field[0]->store(buff, strlen(buff), cs);
  if (db)
    table->field[i++]->store(db, strlen(db), cs);
  if (t_name)
    table->field[i++]->store(t_name, strlen(t_name), cs);
  if (column)
    table->field[i++]->store(column, col_length, cs);
  table->field[i++]->store(priv, priv_length, cs);
  table->field[i]->store(is_grantable, strlen(is_grantable), cs);
  table->file->write_row(table->record[0]);
}


int fill_schema_user_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  uint counter;
  ACL_USER *acl_user;
  ulong want_access;

  char buff[100];
  TABLE *table= tables->table;
  DBUG_ENTER("fill_schema_user_privileges");
  for (counter=0 ; counter < acl_users.elements ; counter++)
  {
    const char *user,*host, *is_grantable="YES";
    acl_user=dynamic_element(&acl_users,counter,ACL_USER*);
    if (!(user=acl_user->user))
      user= "";
    if (!(host=acl_user->host.hostname))
      host= "";
    want_access= acl_user->access;
    if (!(want_access & GRANT_ACL))
      is_grantable= "NO";

    strxmov(buff,"'",user,"'@'",host,"'",NullS);
    if (!(want_access & ~GRANT_ACL))
      update_schema_privilege(table, buff, 0, 0, 0, 0, "USAGE", 5, is_grantable);
    else
    {
      uint priv_id;
      ulong j,test_access= want_access & ~GRANT_ACL;
      for (priv_id=0, j = SELECT_ACL;j <= GLOBAL_ACLS; priv_id++,j <<= 1)
      {
	if (test_access & j)
          update_schema_privilege(table, buff, 0, 0, 0, 0, 
                                  command_array[priv_id],
                                  command_lengths[priv_id], is_grantable);
      }
    }
  }
  DBUG_RETURN(0);
#else
  return(0);
#endif
}


int fill_schema_schema_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  uint counter;
  ACL_DB *acl_db;
  ulong want_access;
  char buff[100];
  TABLE *table= tables->table;
  DBUG_ENTER("fill_schema_schema_privileges");

  for (counter=0 ; counter < acl_dbs.elements ; counter++)
  {
    const char *user, *host, *is_grantable="YES";

    acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);
    if (!(user=acl_db->user))
      user= "";
    if (!(host=acl_db->host.hostname))
      host= "";

    want_access=acl_db->access;
    if (want_access)
    {
      if (!(want_access & GRANT_ACL))
      {
        is_grantable= "NO";
      }
      strxmov(buff,"'",user,"'@'",host,"'",NullS);
      if (!(want_access & ~GRANT_ACL))
        update_schema_privilege(table, buff, acl_db->db, 0, 0,
                                0, "USAGE", 5, is_grantable);
      else
      {
        int cnt;
        ulong j,test_access= want_access & ~GRANT_ACL;
        for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
          if (test_access & j)
            update_schema_privilege(table, buff, acl_db->db, 0, 0, 0,
                                    command_array[cnt], command_lengths[cnt],
                                    is_grantable);
      }
    }
  }
  DBUG_RETURN(0);
#else
  return (0);
#endif
}


int fill_schema_table_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  DBUG_ENTER("fill_schema_table_privileges");

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) hash_element(&column_priv_hash,
							  index);
    if (!(user=grant_table->user))
      user= "";
    ulong table_access= grant_table->privs;
    if (table_access != 0)
    {
      ulong test_access= table_access & ~GRANT_ACL;
      if (!(table_access & GRANT_ACL))
        is_grantable= "NO";

      strxmov(buff,"'",user,"'@'",grant_table->orig_host,"'",NullS);
      if (!test_access)
        update_schema_privilege(table, buff, grant_table->db, grant_table->tname,
                                0, 0, "USAGE", 5, is_grantable);
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
            update_schema_privilege(table, buff, grant_table->db, 
                                    grant_table->tname, 0, 0, command_array[cnt],
                                    command_lengths[cnt], is_grantable);
        }
      }
    }
  }
  DBUG_RETURN(0);
#else
  return (0);
#endif
}


int fill_schema_column_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  DBUG_ENTER("fill_schema_table_privileges");

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) hash_element(&column_priv_hash,
							  index);
    if (!(user=grant_table->user))
      user= "";
    ulong table_access= grant_table->cols;
    if (table_access != 0)
    {
      if (!(grant_table->privs & GRANT_ACL))
        is_grantable= "NO";

      ulong test_access= table_access & ~GRANT_ACL;
      strxmov(buff,"'",user,"'@'",grant_table->orig_host,"'",NullS);
      if (!test_access)
        continue;
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
          {
            for (uint col_index=0 ;
                 col_index < grant_table->hash_columns.records ;
                 col_index++)
            {
              GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
                hash_element(&grant_table->hash_columns,col_index);
              if ((grant_column->rights & j) && (table_access & j))
                  update_schema_privilege(table, buff, grant_table->db,
                                          grant_table->tname,
                                          grant_column->column,
                                          grant_column->key_length,
                                          command_array[cnt],
                                          command_lengths[cnt], is_grantable);
            }
          }
        }
      }
    }
  }
  DBUG_RETURN(0);
#else
  return (0);
#endif
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
/*
  fill effective privileges for table

  SYNOPSIS
    fill_effective_table_privileges()
    thd     thread handler
    grant   grants table descriptor
    db      db name
    table   table name
*/

void fill_effective_table_privileges(THD *thd, GRANT_INFO *grant,
                                     const char *db, const char *table)
{
  /* --skip-grants */
  if (!initialized)
  {
    grant->privilege= ~NO_ACCESS;             // everything is allowed
    return;
  }

  /* global privileges */
  grant->privilege= thd->master_access;

  /* db privileges */
  grant->privilege|= acl_get(thd->host, thd->ip, thd->priv_user, db, 0);

  if (!grant_option)
    return;

  /* table privileges */
  if (grant->version != grant_version)
  {
    rw_rdlock(&LOCK_grant);
    grant->grant_table=
      table_hash_search(thd->host, thd->ip, db,
			thd->priv_user,
			table, 0);              /* purecov: inspected */
    grant->version= grant_version;              /* purecov: inspected */
    rw_unlock(&LOCK_grant);
  }
  if (grant->grant_table != 0)
  {
    grant->privilege|= grant->grant_table->privs;
  }
}
#endif
