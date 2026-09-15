#ifndef E_NETWORKMANAGER_ONLINE_H
#define E_NETWORKMANAGER_ONLINE_H

#include <Eina.h>

typedef struct Enm_Online_Check Enm_Online_Check;

typedef void (*Enm_Online_Cb)(void *data, Eina_Bool online);

/* Main-loop API.  The callback runs only when the check confirms Internet
 * access.  Free the check before freeing its callback data. */
Enm_Online_Check *enm_online_check_new(Enm_Online_Cb cb, void *data);
void enm_online_check_free(Enm_Online_Check *check);

/* Supply the latest network state to discard obsolete responses.  This does
 * not start a request; the caller starts it after connectivity settles. */
void enm_online_check_state_set(Enm_Online_Check *check, Eina_Bool connected,
                                Eina_Bool online);
void enm_online_check_start(Enm_Online_Check *check);
void enm_online_check_stop(Enm_Online_Check *check);

#endif
