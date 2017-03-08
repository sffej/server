/*
   Copyright (c) 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <mysql/plugin_auth.h>
#include "common.h"

#define PASSWORD_LEN_BUF 44 /* base64 of 32 bytes */
#define PASSWORD_LEN 43     /* we won't store the last byte, padding '=' */

/************************** SERVER *************************************/

static int loaded= 0;

static int auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  unsigned long long out_len;
  unsigned int i;
  int pkt_len;
  unsigned char *pkt, reply[CRYPTO_BYTES + NONCE_BYTES], out[NONCE_BYTES];
  unsigned long *nonce= (unsigned long*)(reply + CRYPTO_BYTES);
  unsigned char pk[CRYPTO_PUBLICKEYBYTES];
  char pw[PASSWORD_LEN_BUF];

  /* prepare the pk */
  if (info->auth_string_length != PASSWORD_LEN)
    return CR_AUTH_USER_CREDENTIALS;
  memcpy(pw, info->auth_string, PASSWORD_LEN);
  pw[PASSWORD_LEN]= '=';
  if (base64_decode(pw, PASSWORD_LEN_BUF, pk, NULL, 0) != CRYPTO_PUBLICKEYBYTES)
    return CR_AUTH_USER_CREDENTIALS;

  info->password_used= PASSWORD_USED_YES;

  /* prepare random nonce */
  for (i=0; i < NONCE_BYTES / sizeof(nonce[0]); i++)
    nonce[i]= thd_rnd(info->thd) * ~0UL;

  /* send it */
  if (vio->write_packet(vio, reply + CRYPTO_BYTES, NONCE_BYTES))
    return CR_AUTH_HANDSHAKE;

  /* read the signature */
  if ((pkt_len= vio->read_packet(vio, &pkt)) != CRYPTO_BYTES)
    return CR_AUTH_HANDSHAKE;
  memcpy(reply, pkt, CRYPTO_BYTES);

  if (crypto_sign_open(out, &out_len, reply, sizeof(reply), pk))
    return CR_ERROR;

  return CR_OK;
}

static struct st_mysql_auth info =
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "client_ed25519",
 auth
};

static int init(void *p __attribute__((unused)))
{
  loaded= 1;
  return 0;
}

static int deinit(void *p __attribute__((unused)))
{
  loaded= 0;
  return 0;
}

maria_declare_plugin(ed25519)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "ed25519",
  "Sergei Golubchik",
  "Elliptic curve ED25519 based authentication",
  PLUGIN_LICENSE_GPL,
  init,
  deinit,
  0x0100,
  NULL,
  NULL,
  "1.0-alpha",
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;

/************************** UDF ****************************************/
char *ed25519_password(UDF_INIT *initid __attribute__((unused)),
                       UDF_ARGS *args, char *result, ulong *length,
                       char *is_null, char *error __attribute__((unused)))
{
  unsigned char sk[CRYPTO_SECRETKEYBYTES], pk[CRYPTO_PUBLICKEYBYTES];

  if ((*is_null= !args->args[0]))
    return NULL;

  *length= PASSWORD_LEN;
  pw_to_sk_and_pk(args->args[0], args->lengths[0], sk, pk);
  base64_encode(pk, sizeof(pk), result);
  return result;
}

/*
  At least one of _init/_deinit is needed unless the server is started
  with --allow_suspicious_udfs.
*/
my_bool ed25519_password_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT)
  {
    strcpy(message,"Wrong arguments to ed25519_password()");
    return 1;
  }
  if (!loaded)
  {
    /* cannot work unless the plugin is loaded, we need services. */
    strcpy(message,"Authentication plugin ed25519 is not loaded");
    return 1;
  }
  initid->max_length= PASSWORD_LEN_BUF;
  return 0;
}
