#include "e.h"
#include "e_networkmanager_online.h"

struct Enm_Online_Check
{
   Ecore_Con_Url       *url;
   Eina_Binbuf         *body;
   Ecore_Event_Handler *data_handler;
   Ecore_Event_Handler *complete_handler;
   Enm_Online_Cb        cb;
   void                *cb_data;
   Eina_Bool            url_initialized : 1;
   Eina_Bool            connected : 1;
   Eina_Bool            online : 1;
};

Enm_Online_Check *
enm_online_check_new(Enm_Online_Cb cb, void *data)
{
   Enm_Online_Check *check = calloc(1, sizeof(*check));

   if (!check) return NULL;
   check->cb = cb;
   check->cb_data = data;
   return check;
}

void
enm_online_check_state_set(Enm_Online_Check *check, Eina_Bool connected,
                            Eina_Bool online)
{
   if (!check) return;
   check->connected = connected;
   check->online = online;
}

void
enm_online_check_stop(Enm_Online_Check *check)
{
   if (!check) return;
   E_FREE_FUNC(check->data_handler, ecore_event_handler_del);
   E_FREE_FUNC(check->complete_handler, ecore_event_handler_del);
   E_FREE_FUNC(check->url, ecore_con_url_free);
   E_FREE_FUNC(check->body, eina_binbuf_free);
}

static char *
_enm_refresh_content_decode(const char *content)
{
   Eina_Strbuf *buf = eina_strbuf_new();
   const char *p;
   char *decoded;

   if (!buf) return NULL;
   for (p = content; *p; p++)
     {
        if (*p == '&')
          {
             const char *end = p + 1, *escape;

             if (*end == '#') end++;
             while (isalnum((unsigned char)*end)) end++;
             if (*end == ';')
               {
                  escape = evas_textblock_escape_string_range_get(p, end + 1);
                  if (escape)
                    {
                       if (!eina_strbuf_append(buf, escape)) goto error;
                       p = end;
                       continue;
                    }
               }
          }
        if (!eina_strbuf_append_char(buf, *p)) goto error;
     }
   decoded = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return decoded;

error:
   eina_strbuf_free(buf);
   return NULL;
}

static char *
_enm_refresh_url_get(const char *content)
{ // parse the refresh url string ignoring the initial delay
   char *decoded, *p, *end, *url = NULL;

   // Decode HTML entities, preserving literal '&' in portal query strings.
   decoded = _enm_refresh_content_decode(content);
   if (!decoded) return NULL;
   // skip after initial 3; timeout
   p = strchr(decoded, ';');
   if (!p) goto end;
   p++;
   // skip following shitespace if any
   while (isspace((unsigned char)*p)) p++;
   // look for "url" of url=xxxx
   if (strncasecmp(p, "url", 3)) goto end;
   p += 3;
   // skip whitespace if any
   while (isspace((unsigned char)*p)) p++;
   // fine the = and skip it once found
   if (*p != '=') goto end;
   p++;
   // skip any more whitespace
   while (isspace((unsigned char)*p)) p++;
   end = p + strlen(p);
   // trim whitespace at end by reducing end ptr if any is there
   while ((end > p) && isspace((unsigned char)end[-1])) end--;
   if ((*p == '\'') || (*p == '"'))
     { // trim url from p to end
        if (((end - p) < 2) || (end[-1] != *p)) goto end;
        p++;
        end--;
     }
   // truncate at end fo url string is p
   *end = 0;
   //  url must start with http:// or https:// only
   if ((!strncasecmp(p, "http://", 7) && p[7]) ||
       (!strncasecmp(p, "https://", 8) && p[8]))
     url = strdup(p);
end:
   free(decoded);
   L("NM: Network login portal url: %s", url);
   return url;
}

static Eina_Bool
_enm_refresh_tag_cb(void *data, Eina_Simple_XML_Type type,
                    const char *content, unsigned int offset EINA_UNUSED,
                    unsigned int length)
{
   // parse the html body and look for:
   //   <meta http-equiv="refresh" content="0; url=http://some.url.com" />
   char **url = data;
   const char *p = content, *end = content + length;
   char *refresh = NULL;
   Eina_Bool is_refresh = EINA_FALSE;

   // not open and not empty tag -= ignore
   if ((type != EINA_SIMPLE_XML_OPEN) &&
       (type != EINA_SIMPLE_XML_OPEN_EMPTY)) return EINA_TRUE;
   // if its NOT some <meta ... tag then skip it
   if ((length <= 4) || (!!strncasecmp(p, "meta", 4)) ||
       (!isspace((unsigned char)p[4]))) return EINA_TRUE;
   // skip the "meta" start of the tag
   p += 4;

   // Attribute order, case, and quoting vary in portal HTML. Parse within
   // the tag bounds without putting server-supplied data on the stack.
   while (p < end)
     {
        const char *key, *value;
        size_t key_len, value_len;
        char quote = 0;

        while ((p < end) && isspace((unsigned char)*p)) p++;
        key = p;
        while ((p < end) && !isspace((unsigned char)*p) &&
               (*p != '=') && (*p != '/')) p++;
        key_len = p - key;
        if (!key_len) break;
        while ((p < end) && isspace((unsigned char)*p)) p++;
        if ((p == end) || (*p != '=')) continue;
        p++;
        while ((p < end) && isspace((unsigned char)*p)) p++;
        if ((p < end) && ((*p == '\'') || (*p == '"'))) quote = *p++;
        value = p;
        while ((p < end) &&
               (quote ? (*p != quote) : !isspace((unsigned char)*p))) p++;
        if (quote && (p == end)) break;
        value_len = p - value;
        if (quote) p++;

        if ((key_len == 10) && (!strncasecmp(key, "http-equiv", 10)))
          is_refresh = (value_len == 7) && (!strncasecmp(value, "refresh", 7));
        else if ((key_len == 7) && (!strncasecmp(key, "content", 7)))
          {
             free(refresh);
             refresh = strndup(value, value_len);
          }
     }
   if (is_refresh && refresh) *url = _enm_refresh_url_get(refresh);
   free(refresh);
   return !*url;
}

static Eina_Bool
_enm_online_data_cb(void *data, int type EINA_UNUSED, void *event)
{
   Enm_Online_Check *check = data;
   Ecore_Con_Event_Url_Data *ev = event;

   if (ev->url_con != check->url) return ECORE_CALLBACK_PASS_ON;
   // append data to our binbuf
   if ((ev->size > 0) &&
       (!eina_binbuf_append_length(check->body, ev->data, ev->size)))
     // append failed - abort our online check
     enm_online_check_stop(check);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_enm_online_complete_cb(void *data, int type EINA_UNUSED, void *event)
{
   Enm_Online_Check *check = data;
   Ecore_Con_Event_Url_Complete *ev = event;
   Enm_Online_Cb cb = check->cb;
   void *cb_data = check->cb_data;
   Eina_Bool online = EINA_FALSE;
   char *url = NULL;

   if (ev->url_con != check->url) return ECORE_CALLBACK_PASS_ON;
   // Ignore a response while NM is disconnecting or already reports online.
   if ((ev->status >= 200) && check->connected && !check->online)
     {
        const unsigned char *body = eina_binbuf_string_get(check->body);
        size_t length = eina_binbuf_length_get(check->body);

        // contebnt is "OK" thus we are successfully online
        online = (length == 2) && (!memcmp(body, "OK", 2));
        if (!online && (length <= UINT_MAX)) // we are not online
          eina_simple_xml_parse((const char *)body, length, EINA_TRUE,
                                _enm_refresh_tag_cb, &url);
     }
   enm_online_check_stop(check);
   if (online)
     { // we are online - so just call this callback - no need for browser
        if (cb) cb(cb_data, EINA_TRUE);
     }
   // we have a url meta refresh - open that in a browser
   else if (url) e_util_open(url, NULL);
   // we don't have a meta refreash - just open browser and hope it works
   else if (!url) e_util_open("http://www.enlightenment.org", NULL);
   free(url);
   return ECORE_CALLBACK_PASS_ON;
}

void
enm_online_check_start(Enm_Online_Check *check)
{
   if (!check) return;
   if (!check->url_initialized)
     {
        if (!ecore_con_url_init()) return;
        check->url_initialized = EINA_TRUE;
     }
   // stop prev online check
   enm_online_check_stop(check);
   // new place to store http fetch body data
   check->body = eina_binbuf_new();
   // check enlightenment.org for being online with known results
   check->url =
     ecore_con_url_new("http://www.enlightenment.org/online.php");
   // hanlders for http fetch events
   check->data_handler =
     ecore_event_handler_add(ECORE_CON_EVENT_URL_DATA,
                             _enm_online_data_cb, check);
   check->complete_handler =
     ecore_event_handler_add(ECORE_CON_EVENT_URL_COMPLETE,
                             _enm_online_complete_cb, check);
   if ((!check->body) ||
       (!check->url) ||
       (!check->data_handler) ||
       (!check->complete_handler))
     { // one of the above failed to set up - abort check
        enm_online_check_stop(check);
        return;
     }
   // max timeout to wait for some data
   ecore_con_url_timeout_set(check->url, 15.0);
   if (!ecore_con_url_get(check->url)) enm_online_check_stop(check);
}

void
enm_online_check_free(Enm_Online_Check *check)
{
   if (!check) return;
   enm_online_check_stop(check);
   if (check->url_initialized) ecore_con_url_shutdown();
   free(check);
}
