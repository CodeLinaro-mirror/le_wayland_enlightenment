#include "emix.h"

#include <Ecore.h>
#include <Ecore_File.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/profile.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>
#include <math.h>

/* All PipeWire callbacks run on the Ecore main loop. In particular, monitor
 * streams must not use PW_STREAM_FLAG_RT_PROCESS: Emix callbacks touch Evas. */
typedef enum
{
  SINK,
  SOURCE,
  INPUT,
  OUTPUT
} Node_Type;

typedef struct
{
  union
  {
    Emix_Sink          sink;
    Emix_Source        source;
    Emix_Sink_Input    input;
    Emix_Source_Output output;
  } base;
  Node_Type         type;
  uint32_t          id, device_id, client_id;
  int               profile_device;
  const char       *name, *serial;
  struct pw_node   *proxy;
  struct pw_properties *props;
  struct spa_hook   listener;
  struct pw_stream *monitor;
  struct spa_hook   monitor_listener;
  unsigned int      monitor_count;
  int               volumes[SPA_AUDIO_MAX_CHANNELS];
  Emix_Channel      channels[SPA_AUDIO_MAX_CHANNELS];
  Eina_Bool         announced;
} Node;

typedef struct
{
  uint32_t               id;
  struct pw_client      *proxy;
  struct spa_hook        listener;
  struct pw_client_info *info;
} Client;

typedef struct
{
  Emix_Profile base;
  int          index;
} Profile;

typedef struct
{
  Emix_Port    base;
  int          index, device;
  uint32_t     direction;
  int          devices[SPA_AUDIO_MAX_CHANNELS];
  unsigned int n_devices;
} Route;

typedef struct
{
  Emix_Card         base;
  uint32_t          id;
  struct pw_device *proxy;
  struct spa_hook   listener;
  Eina_List        *routes;
  int               profile;
  Eina_Bool         announced;
} Card;

typedef struct
{
  uint32_t id, input, output;
} Link;

typedef struct
{
  struct pw_loop      *loop;
  struct pw_context   *context;
  struct pw_core      *core;
  struct pw_registry  *registry;
  struct pw_metadata  *metadata;
  struct spa_hook      core_listener, registry_listener, metadata_listener;
  uint32_t             metadata_id;
  Ecore_Fd_Handler    *fd;
  Ecore_Timer         *retry;
  Ecore_Job           *disconnect;
  Ecore_Exe           *server, *session, *pulse;
  Ecore_Event_Handler *exe_del;
  Eina_List           *nodes, *links, *cards, *clients;
  Eina_List           *sinks, *sources, *inputs, *outputs;
  const char          *default_sink, *default_source;
  Emix_Event_Cb        cb;
  const void          *data;
  int                  sync;
  Eina_Bool            ready, starting, session_started;
} Context;

static Context  *ctx;
static void      _shutdown(void);
static Eina_Bool _connect(void *data);
static void      _routes_update(Card *card);

static void
_event(enum Emix_Event event, void *info)
{
  if (ctx->cb) ctx->cb((void *)ctx->data, event, info);
}

static Emix_Volume *
_volume(Node *node)
{
  switch (node->type)
    {
    case SINK:
      return &node->base.sink.volume;
    case SOURCE:
      return &node->base.source.volume;
    case INPUT:
      return &node->base.input.volume;
    case OUTPUT:
      return &node->base.output.volume;
    }
  return NULL;
}

static Eina_Bool *
_mute(Node *node)
{
  switch (node->type)
    {
    case SINK:
      return &node->base.sink.mute;
    case SOURCE:
      return &node->base.source.mute;
    case INPUT:
      return &node->base.input.mute;
    case OUTPUT:
      return &node->base.output.mute;
    }
  return NULL;
}

static Eina_List **
_list(Node *node)
{
  switch (node->type)
    {
    case SINK:
      return &ctx->sinks;
    case SOURCE:
      return &ctx->sources;
    case INPUT:
      return &ctx->inputs;
    case OUTPUT:
      return &ctx->outputs;
    }
  return NULL;
}

static void
_node_event(Node *node, int offset)
{
  static const enum Emix_Event events[] =
    {
      EMIX_SINK_ADDED_EVENT, EMIX_SOURCE_ADDED_EVENT,
      EMIX_SINK_INPUT_ADDED_EVENT, EMIX_SOURCE_OUTPUT_ADDED_EVENT
    };
  _event(events[node->type] + offset, node);
}

static void
_changed(Node *node)
{
  if (node->announced) _node_event(node, 2);
}

static Node *
_node_find(uint32_t id)
{
  Eina_List *l;
  Node      *node;

  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if (node->id == id) return node;
    }
  return NULL;
}

static Card *
_card_find(uint32_t id)
{
  Eina_List *l;
  Card      *card;

  EINA_LIST_FOREACH (ctx->cards, l, card)
    {
      if (card->id == id) return card;
    }
  return NULL;
}

static Client *
_client_find(uint32_t id)
{
  Eina_List *l;
  Client    *client;

  EINA_LIST_FOREACH (ctx->clients, l, client)
    {
      if (client->id == id) return client;
    }
  return NULL;
}

static uint32_t
_id_get(const struct spa_dict *props, const char *key)
{
  const char   *value = spa_dict_lookup(props, key);
  char         *end;
  unsigned long id;

  if (!value) return SPA_ID_INVALID;
  id = strtoul(value, &end, 10);
  if ((end == value) || (*end) || (id >= SPA_ID_INVALID))
    return SPA_ID_INVALID;
  return id;
}

static void
_sync(void)
{
  ctx->sync = pw_core_sync(ctx->core, PW_ID_CORE, ctx->sync);
}

static void
_defaults_update(void)
{
  Eina_List *l;
  Node      *node;
  Eina_Bool  value;

  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if (node->type == SINK)
        {
          value = ctx->default_sink && node->name
                  && !strcmp(ctx->default_sink, node->name);
          if (node->base.sink.default_sink == value) continue;
          node->base.sink.default_sink = value;
        }
      else if (node->type == SOURCE)
        {
          value = ctx->default_source && node->name
                  && !strcmp(ctx->default_source, node->name);
          if (node->base.source.default_source == value) continue;
          node->base.source.default_source = value;
        }
      else continue;
      _changed(node);
    }
}

static void
_links_update(void)
{
  Eina_List *l, *ll;
  Node      *node, *target;
  Link      *link;

  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if ((node->type != INPUT) && (node->type != OUTPUT)) continue;
      target = NULL;
      EINA_LIST_FOREACH (ctx->links, ll, link)
        {
          Node *candidate = NULL;

          if ((node->type == INPUT) && (link->output == node->id))
            {
              candidate = _node_find(link->input);
              if (candidate && (candidate->type == SINK))
                {
                  target = candidate;
                  break;
                }
            }
          if ((node->type == OUTPUT) && (link->input == node->id))
            {
              candidate = _node_find(link->output);
              if (candidate && (candidate->type == SOURCE))
                {
                  target = candidate;
                  break;
                }
            }
        }
      if (node->type == INPUT)
        {
          if (node->base.input.sink == (Emix_Sink *)target) continue;
          node->base.input.sink = (Emix_Sink *)target;
        }
      else
        {
          if (node->base.output.source == (Emix_Source *)target) continue;
          node->base.output.source = (Emix_Source *)target;
        }
      _changed(node);
    }
}

static void
_props_read(Node *node, const struct spa_pod *param)
{
  Emix_Volume               *volume = _volume(node);
  const struct spa_pod_prop *prop;
  float                      values[SPA_AUDIO_MAX_CHANNELS], scalar;
  uint32_t                   channels[SPA_AUDIO_MAX_CHANNELS];
  unsigned int               count, i;
  bool                       mute;

  prop = spa_pod_find_prop(param, NULL, SPA_PROP_mute);
  if (prop && (spa_pod_get_bool(&prop->value, &mute) == 0))
    *_mute(node) = mute;
  prop  = spa_pod_find_prop(param, NULL, SPA_PROP_channelVolumes);
  count = prop ? spa_pod_copy_array(&prop->value, SPA_TYPE_Float, values,
                                    SPA_AUDIO_MAX_CHANNELS)
               : 0;
  if (count)
    {
      volume->channel_count = count;
      for (i = 0; i < count; i++)
        {
          node->volumes[i] = lroundf(cbrtf(fmaxf(0, values[i])) * 100);
        }
    }
  else
    {
      prop = spa_pod_find_prop(param, NULL, SPA_PROP_volume);
      if (prop && (spa_pod_get_float(&prop->value, &scalar) == 0))
        {
          for (i = 0; i < volume->channel_count; i++)
            {
              node->volumes[i] = lroundf(cbrtf(fmaxf(0, scalar)) * 100);
            }
        }
    }
  prop  = spa_pod_find_prop(param, NULL, SPA_PROP_channelMap);
  count = prop ? spa_pod_copy_array(&prop->value, SPA_TYPE_Id, channels,
                                    SPA_AUDIO_MAX_CHANNELS)
               : 0;
  for (i = 0; i < count; i++)
    {
      node->channels[i] =
        spa_debug_type_find_short_name(spa_type_audio_channel, channels[i]);
    }
  _changed(node);
}

static void
_node_param(void *data, int seq EINA_UNUSED, uint32_t id,
            uint32_t index EINA_UNUSED, uint32_t next EINA_UNUSED,
            const struct spa_pod *param)
{
  if ((id == SPA_PARAM_Props) && param) _props_read(data, param);
}

static void
_node_props_update(Node *node)
{
  const struct spa_dict *props = &node->props->dict;
  Client     *client = _client_find(node->client_id);
  const char *name, *icon, *pid;

  name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
  if ((node->type == INPUT) || (node->type == OUTPUT))
    {
      name = spa_dict_lookup(props, PW_KEY_MEDIA_NAME);
      if (!name) name = spa_dict_lookup(props, PW_KEY_APP_NAME);
      icon = spa_dict_lookup(props, PW_KEY_APP_ICON_NAME);
      pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
      /* Native streams keep application metadata on their owning client.
       * Stream properties take precedence (e.g. for pipewire-pulse). */
      if (client && client->info && client->info->props)
        {
          const struct spa_dict *app = client->info->props;

          if (!name) name = spa_dict_lookup(app, PW_KEY_APP_NAME);
          if (!icon) icon = spa_dict_lookup(app, PW_KEY_APP_ICON_NAME);
          if (!pid) pid = spa_dict_lookup(app, PW_KEY_APP_PROCESS_ID);
        }
      if (node->type == INPUT)
        {
          eina_stringshare_replace(&node->base.input.icon, icon);
          node->base.input.pid = pid ? atoi(pid) : 0;
        }
      else
        {
          eina_stringshare_replace(&node->base.output.icon, icon);
          node->base.output.pid = pid ? atoi(pid) : 0;
        }
    }
  if (!name) name = node->name;
  if (!name) name = "PipeWire";
  /* The public structures all begin with their display name. */
  eina_stringshare_replace(&node->base.sink.name, name);
  _changed(node);
}

static void
_node_info(void *data, const struct pw_node_info *info)
{
  Node *node = data;

  /* State/parameter updates can carry an empty, non-NULL props dictionary.
   * Only read it when properties changed, or we lose the descriptive name. */
  if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS)) return;
  if (info->props)
    {
      pw_properties_update(node->props, info->props);
      node->client_id = _id_get(&node->props->dict, PW_KEY_CLIENT_ID);
      _node_props_update(node);
    }
}

static const struct pw_node_events node_events =
{
  PW_VERSION_NODE_EVENTS,
  .info  = _node_info,
  .param = _node_param,
};

static void
_client_info(void *data, const struct pw_client_info *info)
{
  Client    *client = data;
  Eina_List *l;
  Node      *node;

  client->info = pw_client_info_update(client->info, info);
  if (!(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS)) return;
  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if (node->client_id == client->id) _node_props_update(node);
    }
}

static const struct pw_client_events client_events =
{
  PW_VERSION_CLIENT_EVENTS,
  .info = _client_info,
};

static void
_profiles_free(Card *card)
{
  Profile *profile;

  EINA_LIST_FREE (card->base.profiles, profile)
    {
      eina_stringshare_del(profile->base.name);
      eina_stringshare_del(profile->base.description);
      free(profile);
    }
}

static void
_routes_free(Card *card)
{
  Route *route;

  EINA_LIST_FREE (card->routes, route)
    {
      eina_stringshare_del(route->base.name);
      eina_stringshare_del(route->base.description);
      free(route);
    }
}

static void
_routes_update(Card *card)
{
  Eina_List   *l, *ll;
  Node        *node;
  Route       *route;
  unsigned int i;

  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if ((node->type != SINK) || (node->device_id != card->id)) continue;
      eina_list_free(node->base.sink.ports);
      node->base.sink.ports = NULL;
      EINA_LIST_FOREACH (card->routes, ll, route)
        {
          if (route->direction != SPA_DIRECTION_OUTPUT) continue;
          for (i = 0; i < route->n_devices; i++)
            {
              if (route->devices[i] == node->profile_device) break;
            }
          if ((route->device != node->profile_device) &&
              (i == route->n_devices))
            continue;
          node->base.sink.ports = eina_list_append(node->base.sink.ports,
                                                   route);
        }
      _changed(node);
    }
}

static void
_device_param(void *data, int seq EINA_UNUSED, uint32_t id,
              uint32_t index EINA_UNUSED, uint32_t next EINA_UNUSED,
              const struct spa_pod *param)
{
  Card      *card = data;
  Eina_List *l;
  int        idx;

  if (!param) return;
  if ((id == SPA_PARAM_EnumProfile) || (id == SPA_PARAM_Profile))
    {
      Profile    *profile;
      const char *name = NULL, *description = NULL;
      uint32_t    available = SPA_PARAM_AVAILABILITY_unknown;

      if (spa_pod_parse_object
          (param, SPA_TYPE_OBJECT_ParamProfile, NULL,
            SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx),
            SPA_PARAM_PROFILE_name, SPA_POD_OPT_String(&name),
            SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&description),
            SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id(&available)) < 0)
        return;
      if (id == SPA_PARAM_Profile)
        {
          card->profile = idx;
          EINA_LIST_FOREACH (card->base.profiles, l, profile)
            {
              profile->base.active = (profile->index == idx);
            }
        }
      else if (name)
        {
          EINA_LIST_FOREACH (card->base.profiles, l, profile)
            if (profile->index == idx) break;
          if (!profile)
            {
              profile = calloc(1, sizeof(*profile));
              if (!profile) return;
              profile->index = idx;
              card->base.profiles
                = eina_list_append(card->base.profiles, profile);
            }
          eina_stringshare_replace(&profile->base.name, name);
          eina_stringshare_replace(&profile->base.description,
                                   description ? description : name);
          profile->base.plugged = available != SPA_PARAM_AVAILABILITY_no;
          profile->base.active  = card->profile == idx;
        }
      if (card->announced) _event(EMIX_CARD_CHANGED_EVENT, card);
    }
  else if ((id == SPA_PARAM_EnumRoute) || (id == SPA_PARAM_Route))
    {
      Route                *route, *other;
      const char           *name = NULL, *description = NULL;
      const struct spa_pod *devices = NULL, *props = NULL;
      uint32_t              direction = SPA_DIRECTION_OUTPUT;
      uint32_t              available = SPA_PARAM_AVAILABILITY_unknown;
      int                   device    = -1;
      Node                 *node;

      if (spa_pod_parse_object
          (param, SPA_TYPE_OBJECT_ParamRoute, NULL,
            SPA_PARAM_ROUTE_index, SPA_POD_Int(&idx),
            SPA_PARAM_ROUTE_direction, SPA_POD_OPT_Id(&direction),
            SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&device),
            SPA_PARAM_ROUTE_name, SPA_POD_OPT_String(&name),
            SPA_PARAM_ROUTE_description, SPA_POD_OPT_String(&description),
            SPA_PARAM_ROUTE_available, SPA_POD_OPT_Id(&available),
            SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Pod(&devices),
            SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
        return;
      EINA_LIST_FOREACH (card->routes, l, route)
        {
          if (route->index == idx) break;
        }
      if (!route)
        {
          route = calloc(1, sizeof(*route));
          if (!route) return;
          route->index  = idx;
          route->device = -1;
          card->routes  = eina_list_append(card->routes, route);
        }
      if (name) eina_stringshare_replace(&route->base.name, name);
      if (description || name)
        eina_stringshare_replace(&route->base.description,
                                 description ? description : name);
      route->direction      = direction;
      route->base.available = available != SPA_PARAM_AVAILABILITY_no;
      if (devices)
        route->n_devices = spa_pod_copy_array(devices, SPA_TYPE_Int,
                                              route->devices,
                                              SPA_AUDIO_MAX_CHANNELS);
      if (id == SPA_PARAM_Route)
        {
          EINA_LIST_FOREACH (card->routes, l, other)
            if (other->device == device) other->base.active = EINA_FALSE;
          route->device      = device;
          route->base.active = EINA_TRUE;
          if (props)
            {
              EINA_LIST_FOREACH (ctx->nodes, l, node)
                {
                  if ((node->device_id == card->id) &&
                      (node->profile_device == device))
                    _props_read(node, props);
                }
            }
        }
      _routes_update(card);
    }
}

static void
_device_info(void *data, const struct pw_device_info *info)
{
  Card       *card = data;
  uint32_t    i;
  const char *name;

  if (info->props)
    {
      name = spa_dict_lookup(info->props, PW_KEY_DEVICE_DESCRIPTION);
      if (!name) name = spa_dict_lookup(info->props, PW_KEY_DEVICE_NAME);
      if (name) eina_stringshare_replace(&card->base.name, name);
      if (card->announced) _event(EMIX_CARD_CHANGED_EVENT, card);
    }
  if (!(info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS)) return;
  for (i = 0; i < info->n_params; i++)
    {
      uint32_t id = info->params[i].id;

      if (!(info->params[i].flags & SPA_PARAM_INFO_READ)) continue;
      if ((id == SPA_PARAM_EnumProfile) || (id == SPA_PARAM_EnumRoute))
        pw_device_enum_params(card->proxy, 0, id, 0, UINT32_MAX, NULL);
    }
  _sync();
}

static const struct pw_device_events device_events =
{
  PW_VERSION_DEVICE_EVENTS,
  .info  = _device_info,
  .param = _device_param,
};

static int
_metadata_property(void *data EINA_UNUSED, uint32_t subject, const char *key,
                   const char *type EINA_UNUSED, const char *value)
{
  const char    **target;
  struct spa_json iter, object;
  char            name[1024], field[64];

  if (subject != PW_ID_CORE) return 0;
  if (!key)
    {
      eina_stringshare_replace(&ctx->default_sink, NULL);
      eina_stringshare_replace(&ctx->default_source, NULL);
      _defaults_update();
      return 0;
    }
  if      (!strcmp(key, "default.audio.sink")) target = &ctx->default_sink;
  else if (!strcmp(key, "default.audio.source")) target = &ctx->default_source;
  else return 0;
  eina_stringshare_replace(target, NULL);
  if (value)
    {
      spa_json_init(&iter, value, strlen(value));
      if (spa_json_enter_object(&iter, &object) > 0)
        {
          while (spa_json_get_string(&object, field, sizeof(field)) > 0)
            {
              if (spa_json_get_string(&object, name, sizeof(name)) <= 0) break;
              if (!strcmp(field, "name"))
                {
                  eina_stringshare_replace(target, name);
                  break;
                }
            }
        }
    }
  _defaults_update();
  return 0;
}

static const struct pw_metadata_events metadata_events =
{
  PW_VERSION_METADATA_EVENTS,
  .property = _metadata_property,
};

static void
_registry_global(void *data EINA_UNUSED, uint32_t id,
                 uint32_t permissions EINA_UNUSED, const char *type,
                 uint32_t version, const struct spa_dict *props)
{
  const char *name, *media_class;

  if (!props) return;
  if (!strcmp(type, PW_TYPE_INTERFACE_Node))
    {
      Node        *node;
      Node_Type    node_type;
      Emix_Volume *volume;
      uint32_t     params[] = { SPA_PARAM_Props };
      unsigned int i;
      Card        *card;

      media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
      name        = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if ((!media_class) || (name && !strncmp(name, "emix-monitor-", 13)))
        return;
      if (!strcmp(media_class, "Audio/Sink")) node_type = SINK;
      else if (!strcmp(media_class, "Audio/Source") ||
               !strcmp(media_class, "Audio/Source/Virtual"))
        node_type = SOURCE;
      else if (!strcmp(media_class, "Stream/Output/Audio")) node_type = INPUT;
      else if (!strcmp(media_class, "Stream/Input/Audio")) node_type = OUTPUT;
      else return;
      node = calloc(1, sizeof(*node));
      if (!node) return;
      node->props = pw_properties_new_dict(props);
      if (!node->props)
        {
          free(node);
          return;
        }
      node->id   = id;
      node->type = node_type;
      node->name = eina_stringshare_add(name);
      node->serial
        = eina_stringshare_add(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL));
      node->device_id       = _id_get(props, PW_KEY_DEVICE_ID);
      node->client_id       = _id_get(props, PW_KEY_CLIENT_ID);
      node->profile_device  = (int)_id_get(props, "card.profile.device");
      node->base.sink.name  = eina_stringshare_add(name ? name : "PipeWire");
      volume                = _volume(node);
      volume->channel_count = 2;
      volume->volumes       = node->volumes;
      volume->channel_names = node->channels;
      for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
        {
          node->volumes[i]  = 100;
          node->channels[i] = "Unknown";
        }
      node->proxy = pw_registry_bind(ctx->registry, id, type,
                                     MIN(version, PW_VERSION_NODE), 0);
      if (!node->proxy)
        {
          eina_stringshare_del(node->name);
          eina_stringshare_del(node->serial);
          eina_stringshare_del(node->base.sink.name);
          pw_properties_free(node->props);
          free(node);
          return;
        }
      ctx->nodes   = eina_list_append(ctx->nodes, node);
      *_list(node) = eina_list_append(*_list(node), node);
      _node_props_update(node);
      pw_node_add_listener(node->proxy, &node->listener, &node_events, node);
      pw_node_subscribe_params(node->proxy, params,
                               EINA_C_ARRAY_LENGTH(params));
      card = _card_find(node->device_id);
      if (card) _routes_update(card);
      _defaults_update();
      _sync();
    }
  else if (!strcmp(type, PW_TYPE_INTERFACE_Client))
    {
      Client *client = calloc(1, sizeof(*client));

      if (!client) return;
      client->id = id;
      client->proxy = pw_registry_bind(ctx->registry, id, type,
                                       MIN(version, PW_VERSION_CLIENT), 0);
      if (!client->proxy)
        {
          free(client);
          return;
        }
      ctx->clients = eina_list_append(ctx->clients, client);
      pw_client_add_listener(client->proxy, &client->listener,
                             &client_events, client);
      /* Resolve client metadata before announcing newly discovered streams. */
      _sync();
    }
  else if (!strcmp(type, PW_TYPE_INTERFACE_Device))
    {
      Card    *card;
      uint32_t params[] = { SPA_PARAM_Profile, SPA_PARAM_Route };

      media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
      if ((!media_class) || (!!strcmp(media_class, "Audio/Device"))) return;
      card = calloc(1, sizeof(*card));
      if (!card) return;
      card->id        = id;
      card->profile   = -1;
      name            = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);
      card->base.name = eina_stringshare_add(name ? name : "PipeWire");
      card->proxy     = pw_registry_bind(ctx->registry, id, type,
                                         MIN(version, PW_VERSION_DEVICE), 0);
      if (!card->proxy)
        {
          eina_stringshare_del(card->base.name);
          free(card);
          return;
        }
      ctx->cards = eina_list_append(ctx->cards, card);
      pw_device_add_listener(card->proxy, &card->listener, &device_events,
                             card);
      pw_device_subscribe_params(card->proxy, params,
                                 EINA_C_ARRAY_LENGTH(params));
      _sync();
    }
  else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata))
    {
      name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
      if ((ctx->metadata) || (!name) || (!!strcmp(name, "default"))) return;
      ctx->metadata = pw_registry_bind(ctx->registry, id, type,
                                       MIN(version, PW_VERSION_METADATA), 0);
      if (!ctx->metadata) return;
      ctx->metadata_id = id;
      pw_metadata_add_listener(ctx->metadata, &ctx->metadata_listener,
                               &metadata_events, NULL);
      _sync();
    }
  else if (!strcmp(type, PW_TYPE_INTERFACE_Link))
    {
      Link *link = calloc(1, sizeof(*link));

      if (!link) return;
      link->id     = id;
      link->input  = _id_get(props, PW_KEY_LINK_INPUT_NODE);
      link->output = _id_get(props, PW_KEY_LINK_OUTPUT_NODE);
      ctx->links   = eina_list_append(ctx->links, link);
      _links_update();
    }
}

static void
_node_free(Node *node)
{
  if (node->monitor)
    {
      spa_hook_remove(&node->monitor_listener);
      pw_stream_destroy(node->monitor);
    }
  spa_hook_remove(&node->listener);
  pw_proxy_destroy((struct pw_proxy *)node->proxy);
  pw_properties_free(node->props);
  eina_stringshare_del(node->base.sink.name);
  eina_stringshare_del(node->name);
  eina_stringshare_del(node->serial);
  if (node->type == INPUT)  eina_stringshare_del(node->base.input.icon);
  if (node->type == OUTPUT) eina_stringshare_del(node->base.output.icon);
  if (node->type == SINK)   eina_list_free(node->base.sink.ports);
  free(node);
}

static void
_client_free(Client *client)
{
  spa_hook_remove(&client->listener);
  pw_proxy_destroy((struct pw_proxy *)client->proxy);
  if (client->info) pw_client_info_free(client->info);
  free(client);
}

static void
_card_free(Card *card)
{
  spa_hook_remove(&card->listener);
  pw_proxy_destroy((struct pw_proxy *)card->proxy);
  _profiles_free(card);
  _routes_free(card);
  eina_stringshare_del(card->base.name);
  free(card);
}

static void
_registry_remove(void *data EINA_UNUSED, uint32_t id)
{
  Node      *node = _node_find(id);
  Card      *card = _card_find(id);
  Client    *client = _client_find(id);
  Eina_List *l;
  Link      *link;

  if (node)
    {
      ctx->nodes   = eina_list_remove(ctx->nodes, node);
      *_list(node) = eina_list_remove(*_list(node), node);
      _links_update();
      if (node->announced) _node_event(node, 1);
      _node_free(node);
    }
  else if (client)
    {
      ctx->clients = eina_list_remove(ctx->clients, client);
      _client_free(client);
    }
  else if (card)
    {
      /* Drop borrowed route pointers before releasing the device. */
      EINA_LIST_FOREACH (ctx->nodes, l, node)
        {
          if ((node->type == SINK) && (node->device_id == id))
            {
              eina_list_free(node->base.sink.ports);
              node->base.sink.ports = NULL;
              _changed(node);
            }
        }
      ctx->cards = eina_list_remove(ctx->cards, card);
      if (card->announced) _event(EMIX_CARD_REMOVED_EVENT, card);
      _card_free(card);
    }
  else if (ctx->metadata && (ctx->metadata_id == id))
    {
      spa_hook_remove(&ctx->metadata_listener);
      pw_proxy_destroy((struct pw_proxy *)ctx->metadata);
      ctx->metadata = NULL;
      _metadata_property(NULL, PW_ID_CORE, NULL, NULL, NULL);
    }
  else
    {
      EINA_LIST_FOREACH (ctx->links, l, link)
        {
          if (link->id == id)
            {
              ctx->links = eina_list_remove_list(ctx->links, l);
              free(link);
              _links_update();
              break;
            }
        }
    }
}

static const struct pw_registry_events registry_events =
{
  PW_VERSION_REGISTRY_EVENTS,
  .global        = _registry_global,
  .global_remove = _registry_remove,
};

static void
_core_done(void *data EINA_UNUSED, uint32_t id, int seq)
{
  Eina_List *l;
  Node      *node;
  Card      *card;

  if ((id != PW_ID_CORE) || (seq != ctx->sync)) return;
  _links_update();
  /* The lists and initial parameters are complete now. Let the module restore
   * its saved state before added events start updating the saved snapshot. */
  if (!ctx->ready)
    {
      ctx->ready = EINA_TRUE;
      _event(EMIX_READY_EVENT, NULL);
    }
  EINA_LIST_FOREACH (ctx->cards, l, card)
    {
      if (!card->announced)
        {
          card->announced = EINA_TRUE;
          _event(EMIX_CARD_ADDED_EVENT, card);
        }
    }
  EINA_LIST_FOREACH (ctx->nodes, l, node)
    {
      if (!node->announced)
        {
          node->announced = EINA_TRUE;
          _node_event(node, 0);
        }
    }
}

static void
_disconnect(void)
{
  Node *node;
  Card *card;
  Link *link;
  Client *client;

  if (ctx->ready) _event(EMIX_DISCONNECTED_EVENT, NULL);
  ctx->ready = EINA_FALSE;
  while (ctx->inputs) _registry_remove(NULL, ((Node *)ctx->inputs->data)->id);
  while (ctx->outputs) _registry_remove(NULL, ((Node *)ctx->outputs->data)->id);
  while (ctx->nodes)
    {
      node = ctx->nodes->data;
      _registry_remove(NULL, node->id);
    }
  EINA_LIST_FREE (ctx->cards, card)
    {
      if (card->announced) _event(EMIX_CARD_REMOVED_EVENT, card);
      _card_free(card);
    }
  EINA_LIST_FREE (ctx->links, link) free(link);
  EINA_LIST_FREE (ctx->clients, client) _client_free(client);
  if (ctx->metadata)
    {
      spa_hook_remove(&ctx->metadata_listener);
      pw_proxy_destroy((struct pw_proxy *)ctx->metadata);
      ctx->metadata = NULL;
    }
  eina_stringshare_replace(&ctx->default_sink, NULL);
  eina_stringshare_replace(&ctx->default_source, NULL);
  if (ctx->registry)
    {
      spa_hook_remove(&ctx->registry_listener);
      pw_proxy_destroy((struct pw_proxy *)ctx->registry);
      ctx->registry = NULL;
    }
  if (ctx->core)
    {
      spa_hook_remove(&ctx->core_listener);
      pw_core_disconnect(ctx->core);
      ctx->core = NULL;
    }
}

static void
_disconnect_job(void *data EINA_UNUSED)
{
  ctx->disconnect = NULL;
  _disconnect();
  if (!ctx->retry) ctx->retry = ecore_timer_add(1.0, _connect, NULL);
}

static void
_core_error(void *data EINA_UNUSED, uint32_t id, int seq EINA_UNUSED, int res,
            const char *message)
{
  EINA_LOG_WARN("PipeWire: %s (%d)", message, res);
  if ((id == PW_ID_CORE) && ((res == -EPIPE) || (res == -ECONNRESET)))
    {
      if (!ctx->disconnect)
        ctx->disconnect = ecore_job_add(_disconnect_job, NULL);
    }
}

static const struct pw_core_events core_events =
{
  PW_VERSION_CORE_EVENTS,
  .done  = _core_done,
  .error = _core_error,
};

static Eina_Bool
_iterate(void *data EINA_UNUSED, Ecore_Fd_Handler *handler EINA_UNUSED)
{
  pw_loop_iterate(ctx->loop, 0);
  return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
_exe_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
  Ecore_Exe_Event_Del *ev = event;

  if (ev->exe == ctx->server)  ctx->server = NULL;
  if (ev->exe == ctx->session) ctx->session = NULL;
  if (ev->exe == ctx->pulse)   ctx->pulse = NULL;
  return ECORE_CALLBACK_PASS_ON;
}

static Ecore_Exe *
_start(const char *command)
{
  if (!ecore_file_app_installed(command))
    {
      EINA_LOG_WARN("Cannot start %s: executable not installed", command);
      return NULL;
    }
  return ecore_exe_run(command, NULL);
}

static Eina_Bool
_connect(void *data EINA_UNUSED)
{
  ctx->core = pw_context_connect(ctx->context, NULL, 0);
  if (!ctx->core)
    {
      int error = errno;

      EINA_LOG_DBG("PipeWire connection failed: %s", strerror(error));
      if (emix_autostart_get() && (!ctx->starting) &&
          ((error == ENOENT) || (error == ECONNREFUSED) ||
           (error == EHOSTDOWN)))
        {
          /* Never start a local daemon for an explicitly selected remote. */
          if (!getenv("PIPEWIRE_REMOTE"))
            {
              ctx->starting = EINA_TRUE;
              ctx->server   = _start("pipewire");
            }
        }
      return ECORE_CALLBACK_RENEW;
    }
  ctx->retry = NULL;
  pw_core_add_listener(ctx->core, &ctx->core_listener, &core_events, NULL);
  ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
  if (!ctx->registry)
    {
      _disconnect();
      ctx->retry = ecore_timer_add(1.0, _connect, NULL);
      return ECORE_CALLBACK_CANCEL;
    }
  pw_registry_add_listener(ctx->registry, &ctx->registry_listener,
                           &registry_events, NULL);
  _sync();
  /* Only start companions for a daemon started by this backend. Existing
   * sessions retain their own service/session-manager configuration. */
  if (ctx->server && !ctx->session_started)
    {
      ctx->session_started = EINA_TRUE;
      ctx->session         = _start("wireplumber");
      if (!ctx->session) ctx->session = _start("pipewire-media-session");
      ctx->pulse = _start("pipewire-pulse");
    }
  return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
_init(Emix_Event_Cb cb, const void *data)
{
  if (ctx) return EINA_TRUE;
  ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return EINA_FALSE;
  if (!ecore_file_init())
    {
      free(ctx);
      ctx = NULL;
      return EINA_FALSE;
    }
  ctx->cb   = cb;
  ctx->data = data;
  pw_init(NULL, NULL);
  ctx->loop = pw_loop_new(NULL);
  if (!ctx->loop) goto error;
  pw_loop_enter(ctx->loop);
  ctx->context = pw_context_new
    (ctx->loop,
     pw_properties_new(PW_KEY_APP_NAME, "Enlightenment Mixer",
                       PW_KEY_APP_ID, "org.enlightenment.emixer",
                       NULL),
     0);
  if (!ctx->context) goto error;
  ctx->fd = ecore_main_fd_handler_add(pw_loop_get_fd(ctx->loop), ECORE_FD_READ,
                                      _iterate, NULL, NULL, NULL);
  if (!ctx->fd) goto error;
  ctx->exe_del = ecore_event_handler_add(ECORE_EXE_EVENT_DEL, _exe_del, NULL);
  if (!ctx->exe_del) goto error;
  if (_connect(NULL)) ctx->retry = ecore_timer_add(1.0, _connect, NULL);
  return EINA_TRUE;
error:
  _shutdown();
  return EINA_FALSE;
}

static void
_shutdown(void)
{
  if (!ctx) return;
  if (ctx->retry) ecore_timer_del(ctx->retry);
  if (ctx->disconnect) ecore_job_del(ctx->disconnect);
  if (ctx->fd) ecore_main_fd_handler_del(ctx->fd);
  _disconnect();
  if (ctx->exe_del) ecore_event_handler_del(ctx->exe_del);
  /* Terminate only processes we own, never an existing user's service. */
  if (ctx->pulse) ecore_exe_terminate(ctx->pulse);
  if (ctx->session) ecore_exe_terminate(ctx->session);
  if (ctx->server) ecore_exe_terminate(ctx->server);
  if (ctx->context) pw_context_destroy(ctx->context);
  if (ctx->loop)
    {
      pw_loop_leave(ctx->loop);
      pw_loop_destroy(ctx->loop);
    }
  free(ctx);
  ctx = NULL;
  pw_deinit();
  ecore_file_shutdown();
}

static void
_props_set(Node *node, const struct spa_pod *props)
{
  Card      *card;
  Eina_List *l;
  Route     *route;

  if ((!ctx) || (!ctx->core) || (!node)) return;
  card = _card_find(node->device_id);
  if (card)
    {
      EINA_LIST_FOREACH (card->routes, l, route)
        {
          if (route->base.active && (route->device == node->profile_device))
            {
              char                    buffer[4096];
              struct spa_pod_builder  b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
              struct spa_pod         *param;

              param = spa_pod_builder_add_object(&b,
                                                 SPA_TYPE_OBJECT_ParamRoute,
                                                 SPA_PARAM_Route,
                                                 SPA_PARAM_ROUTE_index, SPA_POD_Int(route->index),
                                                 SPA_PARAM_ROUTE_device, SPA_POD_Int(route->device),
                                                 SPA_PARAM_ROUTE_props, SPA_POD_Pod(props), SPA_PARAM_ROUTE_save,
                                                 SPA_POD_Bool(true));
              pw_device_set_param(card->proxy, SPA_PARAM_Route, 0, param);
              return;
            }
        }
    }
  pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, props);
}

static void
_volume_set(Node *node, Emix_Volume *volume)
{
  char                   buffer[2048];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_pod        *param;
  float                  values[SPA_AUDIO_MAX_CHANNELS], value;
  unsigned int           i;

  if ((!volume) || (!volume->volumes) || (!volume->channel_count) ||
      (volume->channel_count > SPA_AUDIO_MAX_CHANNELS))
    return;
  for (i = 0; i < volume->channel_count; i++)
    {
      value     = MAX(0, MIN(150, volume->volumes[i])) / 100.0f;
      values[i] = value * value * value;
    }
  param = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props,
                                     SPA_PARAM_Props, SPA_PROP_channelVolumes,
                                     SPA_POD_Array(sizeof(float),
                                                   SPA_TYPE_Float,
                                                   volume->channel_count,
                                                   values));
  _props_set(node, param);
}

static void
_mute_set(Node *node, Eina_Bool mute)
{
  char                   buffer[128];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_pod        *param;

  param = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props,
                                     SPA_PARAM_Props, SPA_PROP_mute,
                                     SPA_POD_Bool(!!mute));
  _props_set(node, param);
}

static void
_default_set(Node *node, const char *key)
{
  char  *value, *encoded;
  size_t size;

  if ((!ctx) || (!ctx->metadata) || (!node) || (!node->name)) return;
  size    = strlen(node->name) * 6 + 3;
  encoded = malloc(size);
  value   = malloc(size + 16);
  if (encoded && value)
    {
      spa_json_encode_string(encoded, size, node->name);
      snprintf(value, size + 16, "{\"name\":%s}", encoded);
      pw_metadata_set_property(ctx->metadata, PW_ID_CORE, key,
                               "Spa:String:JSON", value);
    }
  free(encoded);
  free(value);
}

static void
_move(Node *node, Node *target)
{
  char value[32];

  if ((!ctx) || (!ctx->metadata) || (!node) || (!target)) return;
  /* WirePlumber observes target.node; target.object identifies the target
   * by its stable object serial on newer session managers. */
  if (target->serial)
    pw_metadata_set_property(ctx->metadata, node->id, "target.object",
                             "Spa:Id", target->serial);
  snprintf(value, sizeof(value), "%u", target->id);
  pw_metadata_set_property(ctx->metadata, node->id, "target.node", "Spa:Id",
                           value);
}

static Eina_Bool
_sink_port_set(Emix_Sink *sink, const Emix_Port *port)
{
  Node                  *node  = (Node *)sink;
  const Route           *route = (const Route *)port;
  Card                  *card;
  char                   buffer[256];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_pod        *param;

  if ((!ctx) || (!node) || (!port) || (!port->available)) return EINA_FALSE;
  card = _card_find(node->device_id);
  if (!card) return EINA_FALSE;
  param = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamRoute,
                                     SPA_PARAM_Route, SPA_PARAM_ROUTE_index,
                                     SPA_POD_Int(route->index),
                                     SPA_PARAM_ROUTE_device,
                                     SPA_POD_Int(node->profile_device),
                                     SPA_PARAM_ROUTE_save,
                                     SPA_POD_Bool(true));
  return pw_device_set_param(card->proxy, SPA_PARAM_Route, 0, param) >= 0;
}

static Eina_Bool
_card_profile_set(Emix_Card *base, const Emix_Profile *profile)
{
  Card                  *card = (Card *)base;
  const Profile         *p    = (const Profile *)profile;
  char                   buffer[256];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_pod        *param;

  if (!ctx || !card || !p) return EINA_FALSE;
  param = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamProfile,
                                     SPA_PARAM_Profile,
                                     SPA_PARAM_PROFILE_index,
                                     SPA_POD_Int(p->index),
                                     SPA_PARAM_PROFILE_save,
                                     SPA_POD_Bool(true));
  return pw_device_set_param(card->proxy, SPA_PARAM_Profile, 0, param) >= 0;
}

static void
_monitor_process(void *data)
{
  Node             *node = data;
  struct pw_buffer *buffer;
  struct spa_data  *d;
  const float      *samples;
  unsigned int      count, offset, size;

  buffer = pw_stream_dequeue_buffer(node->monitor);
  if (!buffer) return;
  if (!buffer->buffer->n_datas) goto done;
  d = &buffer->buffer->datas[0];
  if (!d->data || !d->chunk || !d->maxsize) goto done;
  offset  = MIN(d->chunk->offset, d->maxsize);
  size    = MIN(d->chunk->size, d->maxsize - offset);
  samples = (const float *)((const char *)d->data + offset);
  count   = size / (2 * sizeof(float));
  if (node->type == SINK)
    {
      node->base.sink.mon_num = count;
      node->base.sink.mon_buf = samples;
      _event(EMIX_SINK_MONITOR_EVENT, node);
      node->base.sink.mon_buf = NULL;
      node->base.sink.mon_num = 0;
    }
  else if (node->type == SOURCE)
    {
      node->base.source.mon_num = count;
      node->base.source.mon_buf = samples;
      _event(EMIX_SOURCE_MONITOR_EVENT, node);
      node->base.source.mon_buf = NULL;
      node->base.source.mon_num = 0;
    }
  else if (node->type == INPUT)
    {
      node->base.input.mon_num = count;
      node->base.input.mon_buf = samples;
      _event(EMIX_SINK_INPUT_MONITOR_EVENT, node);
      node->base.input.mon_buf = NULL;
      node->base.input.mon_num = 0;
    }
done:
  pw_stream_queue_buffer(node->monitor, buffer);
}

static const struct pw_stream_events monitor_events =
{
  PW_VERSION_STREAM_EVENTS,
  .process = _monitor_process
};

static void
_monitor_set(Node *node, Eina_Bool enable)
{
  struct pw_properties     *props;
  char                      name[64], target[32], buffer[1024];
  struct spa_pod_builder    b    = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_audio_info_raw info =
    {
      .format   = SPA_AUDIO_FORMAT_F32,
      .rate     = 48000,
      .channels = 2,
      .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }
    };
  const struct spa_pod *param;

  if ((!ctx) || (!ctx->core) || (!node)) return;
  if (!enable)
    {
      if (node->monitor_count) node->monitor_count--;
      if (!node->monitor_count && node->monitor)
        {
          spa_hook_remove(&node->monitor_listener);
          pw_stream_destroy(node->monitor);
          node->monitor = NULL;
        }
      return;
    }
  node->monitor_count++;
  if (node->monitor) return;
  snprintf(name, sizeof(name), "emix-monitor-%u", node->id);
  snprintf(target, sizeof(target), "%u", node->id);
  props = pw_properties_new
  (PW_KEY_NODE_NAME,      name,
   PW_KEY_APP_ID,         "org.enlightenment.emixer",
   PW_KEY_MEDIA_TYPE,     "Audio",
   PW_KEY_MEDIA_CATEGORY, "Capture",
   PW_KEY_MEDIA_ROLE,     "DSP",
   PW_KEY_TARGET_OBJECT,  node->serial ? node->serial : target,
   PW_KEY_NODE_PASSIVE,   "true",
   "stream.monitor",      "true",
   "stream.capture.sink", node->type == SOURCE ? "false" : "true",
   "node.dont-reconnect", "true",
   NULL);
  node->monitor = pw_stream_new(ctx->core, name, props);
  if (!node->monitor) return;
  pw_stream_add_listener(node->monitor, &node->monitor_listener,
                         &monitor_events, node);
  param = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  if (pw_stream_connect(node->monitor, PW_DIRECTION_INPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                        &param, 1) < 0)
    {
      spa_hook_remove(&node->monitor_listener);
      pw_stream_destroy(node->monitor);
      node->monitor = NULL;
    }
}

static int
_max_volume(void)
{
  return 150;
}

static Eina_Bool
_default_support(void)
{
  return ctx && ctx->metadata;
}

static const Eina_List *
_sinks_get(void)
{
  return ctx ? ctx->sinks : NULL;
}

static const Eina_List *
_sources_get(void)
{
  return ctx ? ctx->sources : NULL;
}

static const Eina_List *
_inputs_get(void)
{
  return ctx ? ctx->inputs : NULL;
}

static const Eina_List *
_outputs_get(void)
{
  return ctx ? ctx->outputs : NULL;
}

static const Eina_List *
_cards_get(void)
{
  return ctx ? ctx->cards : NULL;
}

static const Emix_Sink *
_sink_default_get(void)
{
  Eina_List *l;
  Emix_Sink *sink;

  if (!ctx) return NULL;
  EINA_LIST_FOREACH (ctx->sinks, l, sink)
    if (sink->default_sink) return sink;
  return eina_list_data_get(ctx->sinks);
}

static const Emix_Source *
_source_default_get(void)
{
  Eina_List   *l;
  Emix_Source *source;

  if (!ctx) return NULL;
  EINA_LIST_FOREACH (ctx->sources, l, source)
    if (source->default_source) return source;
  return eina_list_data_get(ctx->sources);
}

static void
_sink_default_set(Emix_Sink *s)
{
  _default_set((Node *)s, "default.configured.audio.sink");
}

static void
_source_default_set(Emix_Source *s)
{
  _default_set((Node *)s, "default.configured.audio.source");
}

static void
_sink_volume_set(Emix_Sink *s, Emix_Volume *v)
{
  _volume_set((Node *)s, v);
}

static void
_source_volume_set(Emix_Source *s, Emix_Volume *v)
{
  _volume_set((Node *)s, v);
}

static void
_input_volume_set(Emix_Sink_Input *s, Emix_Volume *v)
{
  _volume_set((Node *)s, v);
}

static void
_output_volume_set(Emix_Source_Output *s, Emix_Volume *v)
{
  _volume_set((Node *)s, v);
}

static void
_sink_mute_set(Emix_Sink *s, Eina_Bool m)
{
  _mute_set((Node *)s, m);
}

static void
_source_mute_set(Emix_Source *s, Eina_Bool m)
{
  _mute_set((Node *)s, m);
}

static void
_input_mute_set(Emix_Sink_Input *s, Eina_Bool m)
{
  _mute_set((Node *)s, m);
}

static void
_output_mute_set(Emix_Source_Output *s, Eina_Bool m)
{
  _mute_set((Node *)s, m);
}

static void
_input_move(Emix_Sink_Input *s, Emix_Sink *t)
{
  _move((Node *)s, (Node *)t);
}

static void
_output_move(Emix_Source_Output *s, Emix_Source *t)
{
  _move((Node *)s, (Node *)t);
}

static void
_sink_monitor(Emix_Sink *s, Eina_Bool m)
{
  _monitor_set((Node *)s, m);
}

static void
_source_monitor(Emix_Source *s, Eina_Bool m)
{
  _monitor_set((Node *)s, m);
}

static void
_input_monitor(Emix_Sink_Input *s, Eina_Bool m)
{
  _monitor_set((Node *)s, m);
}

static Emix_Backend
_pipewire_backend =
{
  _init,
  _shutdown,
  _max_volume,
  _sinks_get,
  _default_support,
  _sink_default_get,
  _sink_default_set,
  _sink_mute_set,
  _sink_volume_set,
  _sink_port_set,
  _default_support,
  _inputs_get,
  _input_mute_set,
  _input_volume_set,
  _input_move,
  _sources_get,
  _default_support,
  _source_default_get,
  _source_default_set,
  _source_mute_set,
  _source_volume_set,
  _outputs_get,
  _output_mute_set,
  _output_volume_set,
  _output_move,
  NULL,
  _cards_get,
  _card_profile_set,
  _sink_monitor,
  _input_monitor,
  _source_monitor,
};

E_API Emix_Backend *
emix_backend_pipewire_get(void)
{
  return &_pipewire_backend;
}

E_API const char *emix_backend_pipewire_name = "PIPEWIRE";
