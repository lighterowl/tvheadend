/*
 *  Electronic Program Guide - Common functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <assert.h>

#include "tvheadend.h"
#include "queue.h"
#include "channels.h"
#include "settings.h"
#include "epg.h"
#include "dvr/dvr.h"
#include "htsp.h"
#include "htsmsg_binary.h"

struct epg_brand_tree   epg_brands;
struct epg_season_tree  epg_seasons;
struct epg_episode_tree epg_episodes;
struct epg_channel_tree epg_channels;

LIST_HEAD(epg_unlinked_channel_list1, epg_channel);
LIST_HEAD(epg_unlinked_channel_list2, channel);
struct epg_unlinked_channel_list1 epg_unlinked_channels1;
struct epg_unlinked_channel_list2 epg_unlinked_channels2;

/* **************************************************************************
 * Comparators
 * *************************************************************************/

static int eb_uri_cmp ( const epg_brand_t *a, const epg_brand_t *b )
{
  return strcmp(a->eb_uri, b->eb_uri);
}

static int es_uri_cmp ( const epg_season_t *a, const epg_season_t *b )
{
  return strcmp(a->es_uri, b->es_uri);
}

static int ee_uri_cmp ( const epg_episode_t *a, const epg_episode_t *b )
{
  return strcmp(a->ee_uri, b->ee_uri);
}

static int ec_uri_cmp ( const epg_channel_t *a, const epg_channel_t *b )
{
  return strcmp(a->ec_uri, b->ec_uri);
}

static int ebc_win_cmp ( const epg_broadcast_t *a, const epg_broadcast_t *b )
{
  if ( a->eb_start < b->eb_start ) return -1;
  if ( a->eb_start >= b->eb_stop  ) return 1;
  return 0;
}

static int ptr_cmp ( void *a, void *b )
{
  return a - b;
}

// TODO: wrong place?
static int epg_channel_match ( epg_channel_t *ec, channel_t *ch )
{
  int ret = 0;
  if ( !strcmp(ec->ec_name, ch->ch_name) ) ret = 1;
  if ( ret ) {
    LIST_REMOVE(ec, ec_ulink);
    LIST_REMOVE(ch, ch_eulink);
    channel_set_epg_source(ch, ec);
    ec->ec_channel = ch;
  }
  return ret;
}

/* **************************************************************************
 * Testing/Debug
 * *************************************************************************/

static void _epg_dump ( void )
{
  epg_brand_t   *eb;
  epg_season_t  *es;
  epg_episode_t *ee;
  epg_channel_t *ec;
  epg_broadcast_t *ebc;
  printf("dump epg\n");

  /* Go down the brand/season/episode */
  RB_FOREACH(eb, &epg_brands, eb_link) {
    printf("BRAND: %p %s\n", eb, eb->eb_uri);
    RB_FOREACH(es, &eb->eb_seasons, es_blink) {
      printf("  SEASON: %p %s %d\n", es, es->es_uri, es->es_number);
      RB_FOREACH(ee, &es->es_episodes, ee_slink) {
        printf("    EPISODE: %p %s %d\n", ee, ee->ee_uri, ee->ee_number);
      }
    }
    RB_FOREACH(ee, &eb->eb_episodes, ee_blink) {
      if ( !ee->ee_season ) printf("  EPISODE: %p %s %d\n", ee, ee->ee_uri, ee->ee_number);
    }
  }
  RB_FOREACH(es, &epg_seasons, es_link) {
    if ( !es->es_brand ) {
      printf("SEASON: %p %s %d\n", es, es->es_uri, es->es_number);
      RB_FOREACH(ee, &es->es_episodes, ee_slink) {
        printf("  EPISODE: %p %s %d\n", ee, ee->ee_uri, ee->ee_number);
      }
    }
  }
  RB_FOREACH(ee, &epg_episodes, ee_link) {
    if ( !ee->ee_brand && !ee->ee_season ) {
      printf("EPISODE: %p %s %d\n", ee, ee->ee_uri, ee->ee_number);
    }
  }
  RB_FOREACH(ec, &epg_channels, ec_link) {
    printf("CHANNEL: %s\n", ec->ec_uri);
    RB_FOREACH(ebc, &ec->ec_schedule, eb_slink) {
      if ( ebc->eb_episode ) {
        printf("  BROADCAST: %s @ %ld to %ld\n", ebc->eb_episode->ee_uri,
               ebc->eb_start, ebc->eb_stop);
      }
    }
  }
}

/* **************************************************************************
 * Setup / Update
 * *************************************************************************/

static int _epg_write ( int fd, htsmsg_t *m )
{
  int ret = 1;
  size_t msglen;
  void *msgdata;
  if (m) {
    int r = htsmsg_binary_serialize(m, &msgdata, &msglen, 0x10000);
    htsmsg_destroy(m);
    if (!r) {
      ssize_t w = write(fd, msgdata, msglen);
      free(msgdata);
      if(w == msglen) ret = 0;
    }
  }
  if(ret) {
    tvhlog(LOG_DEBUG, "epg", "failed to store epg to disk");
    close(fd);
    hts_settings_remove("epgdb");
  }
  return ret;
}

void epg_save ( void )
{
  int fd;
  epg_brand_t   *eb;
  epg_season_t  *es;
  epg_episode_t *ee;
  epg_channel_t *ec;
  epg_broadcast_t *ebc;
  
  printf("EPG save\n");
  // TODO: requires markers in the file or some other means of
  //       determining where the various object types are?
  fd = hts_settings_open_file(1, "epgdb");
  RB_FOREACH(ec,  &epg_channels, ec_link) {
    if (_epg_write(fd, epg_channel_serialize(ec))) return;
  }
  RB_FOREACH(eb,  &epg_brands, eb_link) {
    if (_epg_write(fd, epg_brand_serialize(eb))) return;
  }
  RB_FOREACH(es,  &epg_seasons, es_link) {
    if (_epg_write(fd, epg_season_serialize(es))) return;
  }
  RB_FOREACH(ee,  &epg_episodes, ee_link) {
    if (_epg_write(fd, epg_episode_serialize(ee))) return;
  }
  RB_FOREACH(ec, &epg_channels, ec_link) {
    RB_FOREACH(ebc, &ec->ec_schedule, eb_slink) {
      if (_epg_write(fd, epg_broadcast_serialize(ebc))) return;
    }
  }
}

void epg_init ( void )
{
  struct stat st;
  int fd = hts_settings_open_file(0, "epgdb");
  size_t remain;
  uint8_t *mem, *rp;
  fstat(fd, &st);
  rp = mem = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  //TODO:if ( mem == MAP_FAILED ) return;
  remain = st.st_size;
  while ( remain > 4 ) {
    int msglen = (rp[0] << 24) | (rp[1] << 16) | (rp[2] << 8) | rp[3];
    remain -= 4;
    rp += 4;
    htsmsg_t *m = htsmsg_binary_deserialize(rp, msglen, NULL);
    if(m) {
      //htsmsg_print(m);
      htsmsg_destroy(m);
    }
    rp += msglen;
    remain -= msglen;
  }
  munmap(mem, st.st_size);
  close(fd);
}

void epg_updated ( void )
{
  if (0)_epg_dump();
}

void epg_add_channel ( channel_t *ch )
{
  epg_channel_t *ec;
  LIST_FOREACH(ec, &epg_unlinked_channels1, ec_ulink) {
    if ( epg_channel_match(ec, ch) ) break;
  }
  LIST_INSERT_HEAD(&epg_unlinked_channels2, ch, ch_eulink);
}

void epg_rem_channel ( channel_t *ch )
{
  if ( ch->ch_epg_channel ) {
    ch->ch_epg_channel->ec_channel = NULL;
    // TODO: free the channel?
  } else {
    LIST_REMOVE(ch, ch_eulink);
  }
}


/* **************************************************************************
 * Brand
 * *************************************************************************/

epg_brand_t* epg_brand_find_by_uri 
  ( const char *id, int create, int *save )
{
  epg_brand_t *eb;
  static epg_brand_t* skel = NULL;
  
  lock_assert(&global_lock); // pointless!

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_brand_t));
  skel->eb_uri = (char*)id;

  /* Find */
  if ( !create ) {
    eb = RB_FIND(&epg_brands, skel, eb_link, eb_uri_cmp);
  
  /* Create */
  } else {
    eb = RB_INSERT_SORTED(&epg_brands, skel, eb_link, eb_uri_cmp);
    if ( eb == NULL ) {
      eb   = skel;
      skel = NULL;
      eb->eb_uri = strdup(id);
      *save |= 1;
    }
  }

  return eb;
}

int epg_brand_set_title ( epg_brand_t *brand, const char *title )
{
  int save = 0;
  if ( !brand || !title ) return 0;
  if ( !brand->eb_title || strcmp(brand->eb_title, title) ) {
    if ( brand->eb_title ) free(brand->eb_title);
    brand->eb_title = strdup(title);
    save = 1;
  }
  return save;
}

int epg_brand_set_summary ( epg_brand_t *brand, const char *summary )
{
  int save = 0;
  if ( !brand || !summary ) return 0;
  if ( !brand->eb_summary || strcmp(brand->eb_summary, summary) ) {
    if ( brand->eb_summary ) free(brand->eb_summary);
    brand->eb_summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_brand_set_season_count ( epg_brand_t *brand, uint16_t count )
{
  // TODO: could set only if less?
  int save = 0;
  if ( !brand || !count ) return 0;
  if ( brand->eb_season_count != count ) {
    brand->eb_season_count = count;
    save = 1;
  }
  return save;
}

int epg_brand_add_season ( epg_brand_t *brand, epg_season_t *season, int u )
{
  int save = 0;
  epg_season_t *es;
  if ( !brand || !season ) return 0;
  es = RB_INSERT_SORTED(&brand->eb_seasons, season, es_blink, es_uri_cmp);
  if ( es == NULL ) {
    if ( u ) save |= epg_season_set_brand(season, brand, 0);
    save = 1;
  }
  // TODO?: else assert(es == season)
  return save;
}

int epg_brand_rem_season ( epg_brand_t *brand, epg_season_t *season, int u )
{
  int save = 0;
  epg_season_t *es;
  if ( !brand || !season ) return 0;
  es = RB_FIND(&brand->eb_seasons, season, es_blink, es_uri_cmp);
  if ( es != NULL ) {
    // TODO: assert(es == season)
    if ( u ) save |= epg_season_set_brand(season, NULL, 0); // TODO: this will do nothing
    RB_REMOVE(&brand->eb_seasons, season, es_blink);
    save = 1;
  }
  return save;
}

int epg_brand_add_episode ( epg_brand_t *brand, epg_episode_t *episode, int u )
{
  int save = 0;
  epg_episode_t *ee;
  if ( !brand || !episode ) return 0;
  ee = RB_INSERT_SORTED(&brand->eb_episodes, episode, ee_blink, ee_uri_cmp);
  if ( ee == NULL ) {
    if ( u ) save |= epg_episode_set_brand(episode, brand, 0);
    save = 1;
  }
  // TODO?: else assert(ee == episode)
  return save;
}

int epg_brand_rem_episode ( epg_brand_t *brand, epg_episode_t *episode, int u )
{
  int save = 0;
  epg_episode_t *ee;
  if ( !brand || !episode ) return 0;
  ee = RB_FIND(&brand->eb_episodes, episode, ee_blink, ee_uri_cmp);
  if ( ee != NULL ) {
    // TODO?: assert(ee == episode)
    if ( u ) save |= epg_episode_set_brand(episode, NULL, 0); // TODO: will do nothing
    RB_REMOVE(&brand->eb_episodes, episode, ee_blink);
    save = 1;
  }
  return save;
}

htsmsg_t *epg_brand_serialize ( epg_brand_t *brand )
{
  htsmsg_t *m;
  if ( !brand ) return NULL;
  m = htsmsg_create_map();
  // TODO: ID
  if (brand->eb_uri)
    htsmsg_add_str(m, "uri", brand->eb_uri);
  if (brand->eb_title)
    htsmsg_add_str(m, "title",   brand->eb_title);
  if (brand->eb_summary)
    htsmsg_add_str(m, "summary", brand->eb_summary);
  if (brand->eb_season_count)
    htsmsg_add_u32(m, "season-count", brand->eb_season_count);
  return m;
}

/* **************************************************************************
 * Season
 * *************************************************************************/

epg_season_t* epg_season_find_by_uri 
  ( const char *id, int create, int *save )
{
  epg_season_t *es;
  static epg_season_t* skel = NULL;
  
  lock_assert(&global_lock); // pointless!

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_season_t));
  skel->es_uri = (char*)id;

  /* Find */
  if ( !create ) {
    es = RB_FIND(&epg_seasons, skel, es_link, es_uri_cmp);
  
  /* Create */
  } else {
    es = RB_INSERT_SORTED(&epg_seasons, skel, es_link, es_uri_cmp);
    if ( es == NULL ) {
      es   = skel;
      skel = NULL;
      es->es_uri = strdup(id);
      *save |= 1;
    }
  }

  return es;
}

int epg_season_set_summary ( epg_season_t *season, const char *summary )
{
  int save = 0;
  if ( !season || !summary ) return 0;
  if ( !season->es_summary || strcmp(season->es_summary, summary) ) {
    if ( season->es_summary ) free(season->es_summary);
    season->es_summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_season_set_episode_count ( epg_season_t *season, uint16_t count )
{
  int save = 0;
  if ( !season || !count ) return 0;
  // TODO: should we only update if number is larger
  if ( season->es_episode_count != count ) {
    season->es_episode_count = count;
    save = 1;
  }
  return save;
}

int epg_season_set_number ( epg_season_t *season, uint16_t number )
{
  int save = 0;
  if ( !season || !number ) return 0;
  if ( season->es_number != number ) {
    season->es_number = number;
    save = 1;
  }
  return save;
}

int epg_season_set_brand ( epg_season_t *season, epg_brand_t *brand, int u )
{
  int save = 0;
  if ( !season || !brand ) return 0;
  if ( season->es_brand != brand ) {
    if ( u ) save |= epg_brand_rem_season(season->es_brand, season, 0);
    season->es_brand = brand;
    if ( u ) save |= epg_brand_add_season(season->es_brand, season, 0);
    save = 1;
  }
  return save;
}

int epg_season_add_episode 
  ( epg_season_t *season, epg_episode_t *episode, int u )
{
  int save = 0;
  epg_episode_t *ee;
  if ( !season || !episode ) return 0;
  ee = RB_INSERT_SORTED(&season->es_episodes, episode, ee_slink, ee_uri_cmp);
  if ( ee == NULL ) {
    if ( u ) save |= epg_episode_set_season(episode, season, 0);
    save = 1;
  }
  // TODO?: else assert(ee == episode)
  return save;
}

int epg_season_rem_episode 
  ( epg_season_t *season, epg_episode_t *episode, int u )
{
  int save = 0;
  epg_episode_t *ee;
  if ( !season || !episode ) return 0;
  ee = RB_FIND(&season->es_episodes, episode, ee_slink, ee_uri_cmp);
  if ( ee != NULL ) {
    // TODO?: assert(ee == episode)
    if ( u ) save |= epg_episode_set_season(episode, NULL, 0); // does nothing
    RB_REMOVE(&season->es_episodes, episode, ee_slink);
    save = 1;
  }
  return save;
}

htsmsg_t *epg_season_serialize ( epg_season_t *season )
{
  htsmsg_t *m;
  if (!season) return NULL;
  m = htsmsg_create_map();
  if (season->es_uri)
    htsmsg_add_str(m, "uri", season->es_uri);
  if (season->es_summary)
    htsmsg_add_str(m, "summary", season->es_summary);
  if (season->es_number)
    htsmsg_add_u32(m, "number", season->es_number);
  if (season->es_episode_count)
    htsmsg_add_u32(m, "episode-count", season->es_episode_count);
  if (season->es_brand)
    htsmsg_add_str(m, "brand-id", season->es_brand->eb_uri);
  // TODO: change to ID
  return m;
}

/* **************************************************************************
 * Episode
 * *************************************************************************/

epg_episode_t* epg_episode_find_by_uri
  ( const char *id, int create, int *save )
{
  epg_episode_t *ee;
  static epg_episode_t* skel = NULL;
  
  lock_assert(&global_lock); // pointless!

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_episode_t));
  skel->ee_uri = (char*)id;

  /* Find */
  if ( !create ) {
    ee = RB_FIND(&epg_episodes, skel, ee_link, ee_uri_cmp);
  
  /* Create */
  } else {
    ee = RB_INSERT_SORTED(&epg_episodes, skel, ee_link, ee_uri_cmp);
    if ( ee == NULL ) {
      ee   = skel;
      skel = NULL;
      ee->ee_uri = strdup(id);
      *save |= 1;
    }
  }

  return ee;
}

int epg_episode_set_title ( epg_episode_t *episode, const char *title )
{
  int save = 0;
  if ( !episode || !title ) return 0;
  if ( !episode->ee_title || strcmp(episode->ee_title, title) ) {
    if ( episode->ee_title ) free(episode->ee_title);
    episode->ee_title = strdup(title);
    save = 1;
  }
  return save;
}

int epg_episode_set_subtitle ( epg_episode_t *episode, const char *subtitle )
{
  int save = 0;
  if ( !episode || !subtitle ) return 0;
  if ( !episode->ee_subtitle || strcmp(episode->ee_subtitle, subtitle) ) {
    if ( episode->ee_subtitle ) free(episode->ee_subtitle);
    episode->ee_subtitle = strdup(subtitle);
    save = 1;
  }
  return save;
}

int epg_episode_set_summary ( epg_episode_t *episode, const char *summary )
{
  int save = 0;
  if ( !episode || !summary ) return 0;
  if ( !episode->ee_summary || strcmp(episode->ee_summary, summary) ) {
    if ( episode->ee_summary ) free(episode->ee_summary);
    episode->ee_summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_episode_set_description ( epg_episode_t *episode, const char *desc )
{
  int save = 0;
  if ( !episode || !desc ) return 0;
  if ( !episode->ee_description || strcmp(episode->ee_description, desc) ) {
    if ( episode->ee_description ) free(episode->ee_description);
    episode->ee_description = strdup(desc);
    save = 1;
  }
  return save;
}

int epg_episode_set_number ( epg_episode_t *episode, uint16_t number )
{
  int save = 0;
  if ( !episode || !number ) return 0;
  if ( episode->ee_number != number ) {
    episode->ee_number = number;
    save = 1;
  }
  return save;
}

int epg_episode_set_part ( epg_episode_t *episode, uint16_t part, uint16_t count )
{
  int save = 0;
  // TODO: could treat part/count independently
  if ( !episode || !part || !count ) return 0;
  if ( episode->ee_part_number != part ) {
    episode->ee_part_number = part;
    save |= 1;
  }
  if ( episode->ee_part_count != count ) {
    episode->ee_part_count = count;
    save |= 1;
  }
  return save;
}

int epg_episode_set_brand ( epg_episode_t *episode, epg_brand_t *brand, int u )
{
  int save = 0;
  if ( !episode || !brand ) return 0;
  if ( episode->ee_brand != brand ) {
    if ( u ) save |= epg_brand_rem_episode(episode->ee_brand, episode, 0); /// does nothing
    episode->ee_brand = brand;
    if ( u ) save |= epg_brand_add_episode(episode->ee_brand, episode, 0);
    save |= 1;
  }
  return save;
}

int epg_episode_set_season ( epg_episode_t *episode, epg_season_t *season, int u )
{
  int save = 0;
  if ( !episode || !season ) return 0;
  if ( episode->ee_season != season ) {
    if ( u ) save |= epg_season_rem_episode(episode->ee_season, episode, 0);
    episode->ee_season = season;
    if ( u && episode->ee_season ) {
      save |= epg_season_add_episode(episode->ee_season, episode, 0);
      save |= epg_brand_add_episode(episode->ee_season->es_brand, episode, 0);
      // TODO: is this the right place?
    }
    save = 1;
  }
  return save;
}

int epg_episode_add_broadcast 
  ( epg_episode_t *episode, epg_broadcast_t *broadcast, int u )
{
  int save = 0;
  epg_broadcast_t *eb;
  if ( !episode || !broadcast ) return 0;
  eb = RB_INSERT_SORTED(&episode->ee_broadcasts, broadcast, eb_elink, ptr_cmp);
  if ( eb == NULL ) {
    if ( u ) save |= epg_broadcast_set_episode(broadcast, episode, 0);
    save = 1;
  }
  return save;
}

int epg_episode_rem_broadcast
  ( epg_episode_t *episode, epg_broadcast_t *broadcast, int u )
{
  int save = 0;
  epg_broadcast_t *eb;
  if ( !episode || !broadcast ) return 0;
  eb = RB_FIND(&episode->ee_broadcasts, broadcast, eb_elink, ptr_cmp);
  if ( eb != NULL ) {
    if ( u ) save |= epg_broadcast_set_episode(broadcast, episode, 0);
    RB_REMOVE(&episode->ee_broadcasts, broadcast, eb_elink);
    save = 1;
  }
  return save;
}

int epg_episode_get_number_onscreen
  ( epg_episode_t *episode, char *buf, int len )
{
  int i = 0;
  if ( episode->ee_number ) {
    // TODO: add counts
    if ( episode->ee_season && episode->ee_season->es_number ) {
      i += snprintf(&buf[i], len-i, "Season %d ",
                    episode->ee_season->es_number);
    }
    i += snprintf(&buf[i], len-i, "Episode %d", episode->ee_number);
  }
  return i;
}

htsmsg_t *epg_episode_serialize ( epg_episode_t *episode )
{
  htsmsg_t *m;
  if (!episode) return NULL;
  m = htsmsg_create_map();
  if (episode->ee_uri)
    htsmsg_add_str(m, "uri", episode->ee_uri);
  if (episode->ee_title)
    htsmsg_add_str(m, "title", episode->ee_title);
  if (episode->ee_subtitle)
    htsmsg_add_str(m, "subtitle", episode->ee_subtitle);
  if (episode->ee_summary)
    htsmsg_add_str(m, "summary", episode->ee_summary);
  if (episode->ee_description)
    htsmsg_add_str(m, "description", episode->ee_description);
  if (episode->ee_number)
    htsmsg_add_u32(m, "number", episode->ee_number);
  if (episode->ee_part_count && episode->ee_part_count) {
    htsmsg_add_u32(m, "part-number", episode->ee_part_number);
    htsmsg_add_u32(m, "part-count", episode->ee_part_count);
  }
  // TODO: use ids
  if (episode->ee_brand)
    htsmsg_add_str(m, "brand-id", episode->ee_brand->eb_uri);
  if (episode->ee_season)
    htsmsg_add_str(m, "season-id", episode->ee_season->es_uri);
  return m;
}

/* **************************************************************************
 * Broadcast
 * *************************************************************************/

static int _epg_broadcast_idx = 0;

// Note: will find broadcast playing at this time (not necessarily
//       one that starts at this time)
//
// Note: do we need to pass in stop?
epg_broadcast_t* epg_broadcast_find_by_time 
  ( epg_channel_t *channel, time_t start, time_t stop, int create, int *save )
{
  epg_broadcast_t *eb;
  static epg_broadcast_t *skel = NULL;

  if ( !channel || !start || !stop ) return 0;
  if ( stop < start ) return 0;
  
  lock_assert(&global_lock); // pointless!

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_broadcast_t));
  skel->eb_channel = channel;
  skel->eb_start   = start;
  skel->eb_stop    = stop;
  skel->eb_id      = _epg_broadcast_idx;
  
  /* Find */
  if ( !create ) {
    eb = RB_FIND(&channel->ec_schedule, skel, eb_slink, ebc_win_cmp);

  /* Create */
  } else {
    eb = RB_INSERT_SORTED(&channel->ec_schedule, skel, eb_slink, ebc_win_cmp);
    if ( eb == NULL ) {
      eb   = skel;
      skel = NULL;
      *save |= 1;
      _epg_broadcast_idx++;
    }
  }

  return eb;
}

epg_broadcast_t *epg_broadcast_find_by_id ( int id )
{
  // TODO: do this properly
  epg_channel_t *ec;
  epg_broadcast_t *ebc;
  RB_FOREACH(ec, &epg_channels, ec_link) {
    RB_FOREACH(ebc, &ec->ec_schedule, eb_slink) {
      if (ebc->eb_id == id) return ebc;
    }
  }
  return NULL;
}

int epg_broadcast_set_episode 
  ( epg_broadcast_t *broadcast, epg_episode_t *episode, int u )
{
  int save = 0;
  if ( !broadcast || !episode ) return 0;
  if ( broadcast->eb_episode != episode ) {
    broadcast->eb_episode = episode;
    if ( u ) save |= epg_episode_add_broadcast(episode, broadcast, 0);
    save = 1;
  }
  return save;
}

epg_broadcast_t *epg_broadcast_get_next ( epg_broadcast_t *broadcast )
{
  if ( !broadcast ) return NULL;
  return RB_NEXT(broadcast, eb_slink);
}

htsmsg_t *epg_broadcast_serialize ( epg_broadcast_t *broadcast )
{
  htsmsg_t *m;
  if (!broadcast) return NULL;
  m = htsmsg_create_map();
  htsmsg_add_u32(m, "id", broadcast->eb_id);
  htsmsg_add_u32(m, "start", broadcast->eb_start);
  htsmsg_add_u32(m, "stop", broadcast->eb_stop);
  if (broadcast->eb_dvb_id)
    htsmsg_add_u32(m, "dvb-id", broadcast->eb_dvb_id);
  // TODO: add other metadata fields
  // TODO: use ID
  if (broadcast->eb_episode)
    htsmsg_add_str(m, "episode-id", broadcast->eb_episode->ee_uri);
  return m;
}

/* **************************************************************************
 * Channel
 * *************************************************************************/

static void _epg_channel_link ( epg_channel_t *ec )
{
  channel_t *ch;
  LIST_FOREACH(ch, &epg_unlinked_channels2, ch_eulink) {
    if ( epg_channel_match(ec, ch) ) break;
  }
}

epg_channel_t* epg_channel_find_by_uri
  ( const char *id, int create, int *save )
{
  epg_channel_t *ec;
  static epg_channel_t *skel = NULL;
  
  lock_assert(&global_lock); // pointless!

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_channel_t));
  skel->ec_uri = (char*)id;

  /* Find */
  if ( !create ) {
    ec = RB_FIND(&epg_channels, skel, ec_link, ec_uri_cmp);
  
  /* Create */
  } else {
    ec = RB_INSERT_SORTED(&epg_channels, skel, ec_link, ec_uri_cmp);
    if ( ec == NULL ) {
      ec   = skel;
      skel = NULL;
      ec->ec_uri = strdup(id);
      LIST_INSERT_HEAD(&epg_unlinked_channels1, ec, ec_ulink);
      *save |= 1;
    }
  }

  return ec;
}

int epg_channel_set_name ( epg_channel_t *channel, const char *name )
{
  int save = 0;
  if ( !channel || !name ) return 0;
  if ( !channel->ec_name || strcmp(channel->ec_name, name) ) {
    channel->ec_name = strdup(name);
    _epg_channel_link(channel);
    // TODO: will this cope with an already linked channel?
    save = 1;
  }
  return save;
}

epg_broadcast_t *epg_channel_get_current_broadcast ( epg_channel_t *channel )
{
  // TODO: its not really the head!
  if ( !channel ) return NULL;
  return RB_FIRST(&channel->ec_schedule);
}

htsmsg_t *epg_channel_serialize ( epg_channel_t *channel )
{
  htsmsg_t *m;
  if (!channel) return NULL;
  m = htsmsg_create_map();
  if (channel->ec_uri)
    htsmsg_add_str(m, "uri", channel->ec_uri);
  if (channel->ec_channel)
    htsmsg_add_u32(m, "channel-id", channel->ec_channel->ch_id);
  // TODO: other data
  return m;
}

/* **************************************************************************
 * Querying
 * *************************************************************************/

static void _eqr_add ( epg_query_result_t *eqr, epg_broadcast_t *e )
{
  /* More space */
  if ( eqr->eqr_entries == eqr->eqr_alloced ) {
    eqr->eqr_alloced = MAX(100, eqr->eqr_alloced * 2);
    eqr->eqr_array   = realloc(eqr->eqr_array, 
                               eqr->eqr_alloced * sizeof(epg_broadcast_t));
  }
  
  /* Store */
  eqr->eqr_array[eqr->eqr_entries++] = e;
  // TODO: ref counting
}

static void _eqr_add_channel 
  ( epg_query_result_t *eqr, epg_channel_t *ec )
{
  // TODO: add other searching
  epg_broadcast_t *ebc;
  RB_FOREACH(ebc, &ec->ec_schedule, eb_slink) {
    if ( ebc->eb_episode ) _eqr_add(eqr, ebc);
  }
}

void epg_query0
  ( epg_query_result_t *eqr, channel_t *channel, channel_tag_t *tag,
    uint8_t contentgroup, const char *title )
{
  epg_channel_t *ec;

  /* Clear (just incase) */
  memset(eqr, 0, sizeof(epg_query_result_t));

  /* All channels */
  if (!channel) {
    RB_FOREACH(ec, &epg_channels, ec_link) {
      _eqr_add_channel(eqr, ec);
    }

  /* Single channel */
  } else if ( channel->ch_epg_channel ) {
    _eqr_add_channel(eqr, channel->ch_epg_channel);
  }

  return;
}

void epg_query(epg_query_result_t *eqr, const char *channel, const char *tag,
	       const char *contentgroup, const char *title)
{
  channel_t *ch = NULL;
  if (channel) ch = channel_find_by_name(channel, 0, 0);
  epg_query0(eqr, ch, NULL, 0, title);
}

void epg_query_free(epg_query_result_t *eqr)
{
  free(eqr->eqr_array);
  // TODO: reference counting
}

static int _epg_sort_start_ascending ( const void *a, const void *b )
{
  return (*(epg_broadcast_t**)a)->eb_start - (*(epg_broadcast_t**)b)->eb_start;
}

void epg_query_sort(epg_query_result_t *eqr)
{
  qsort(eqr->eqr_array, eqr->eqr_entries, sizeof(epg_broadcast_t*),
        _epg_sort_start_ascending);
}
