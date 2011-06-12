/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Simple module to facilitate twitter functionality.                       *
*                                                                           *
*  Copyright 2009 Geert Mulders <g.c.w.m.mulders@gmail.com>                 *
*                                                                           *
*  This library is free software; you can redistribute it and/or            *
*  modify it under the terms of the GNU Lesser General Public               *
*  License as published by the Free Software Foundation, version            *
*  2.1.                                                                     *
*                                                                           *
*  This library is distributed in the hope that it will be useful,          *
*  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU        *
*  Lesser General Public License for more details.                          *
*                                                                           *
*  You should have received a copy of the GNU Lesser General Public License *
*  along with this library; if not, write to the Free Software Foundation,  *
*  Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA           *
*                                                                           *
****************************************************************************/

/* For strptime(): */
#if(__sun)
#else
#define _XOPEN_SOURCE
#endif

#include "twitter_http.h"
#include "twitter.h"
#include "bitlbee.h"
#include "url.h"
#include "misc.h"
#include "base64.h"
#include "xmltree.h"
#include "twitter_lib.h"
#include <ctype.h>
#include <errno.h>

/* GLib < 2.12.0 doesn't have g_ascii_strtoll(), work around using system strtoll(). */
/* GLib < 2.12.4 can be buggy: http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=488013 */
#if !GLIB_CHECK_VERSION(2,12,5)
#include <stdlib.h>
#include <limits.h>
#define g_ascii_strtoll strtoll
#endif

#define TXL_STATUS 1
#define TXL_USER 2
#define TXL_ID 3

struct twitter_xml_list {
	int type;
	gint64 next_cursor;
	GSList *list;
	gpointer data;
};

struct twitter_xml_user {
	char *name;
	char *screen_name;
};

struct twitter_xml_status {
	time_t created_at;
	char *text;
	struct twitter_xml_user *user;
	guint64 id;
};

static void twitter_groupchat_init(struct im_connection *ic);

/**
 * Frees a twitter_xml_user struct.
 */
static void txu_free(struct twitter_xml_user *txu)
{
	if (txu == NULL)
		return;
	g_free(txu->name);
	g_free(txu->screen_name);
	g_free(txu);
}


/**
 * Frees a twitter_xml_status struct.
 */
static void txs_free(struct twitter_xml_status *txs)
{
	g_free(txs->text);
	txu_free(txs->user);
	g_free(txs);
}

/**
 * Free a twitter_xml_list struct.
 * type is the type of list the struct holds.
 */
static void txl_free(struct twitter_xml_list *txl)
{
	GSList *l;
	if (txl == NULL)
		return;
	for ( l = txl->list; l ; l = g_slist_next(l) )
		if (txl->type == TXL_STATUS)
			txs_free((struct twitter_xml_status *)l->data);
		else if (txl->type == TXL_ID)
			g_free(l->data);
	g_slist_free(txl->list);
	g_free(txl);
}

/**
 * Add a buddy if it is not allready added, set the status to logged in.
 */
static void twitter_add_buddy(struct im_connection *ic, char *name, const char *fullname)
{
	struct twitter_data *td = ic->proto_data;

	// Check if the buddy is allready in the buddy list.
	if (!bee_user_by_handle( ic->bee, ic, name ))
	{
		char *mode = set_getstr(&ic->acc->set, "mode");
		
		// The buddy is not in the list, add the buddy and set the status to logged in.
		imcb_add_buddy( ic, name, NULL );
		imcb_rename_buddy( ic, name, fullname );
		if (g_strcasecmp(mode, "chat") == 0)
		{
			/* Necessary so that nicks always get translated to the
			   exact Twitter username. */
			imcb_buddy_nick_hint( ic, name, name );
			imcb_chat_add_buddy( td->home_timeline_gc, name );
		}
		else if (g_strcasecmp(mode, "many") == 0)
			imcb_buddy_status( ic, name, OPT_LOGGED_IN, NULL, NULL );
	}
}

/* Warning: May return a malloc()ed value, which will be free()d on the next
   call. Only for short-term use. */
static char *twitter_parse_error(struct http_request *req)
{
	static char *ret = NULL;
	struct xt_parser *xp = NULL;
	struct xt_node *node;
	
	g_free(ret);
	ret = NULL;
	
	if (req->body_size > 0)
	{
		xp = xt_new(NULL, NULL);
		xt_feed(xp, req->reply_body, req->body_size);
		
		if ((node = xt_find_node(xp->root, "hash")) &&
		    (node = xt_find_node(node->children, "error")) &&
		    node->text_len > 0)
		{
			ret = g_strdup_printf("%s (%s)", req->status_string, node->text);
			xt_free(xp);
			return ret;
		}
		
		xt_free(xp);
	}
	
	return req->status_string;
}

static void twitter_http_get_friends_ids(struct http_request *req);

/**
 * Get the friends ids.
 */
void twitter_get_friends_ids(struct im_connection *ic, gint64 next_cursor)
{
	// Primitive, but hey! It works...	
	char* args[2];
	args[0] = "cursor";
	args[1] = g_strdup_printf ("%lld", (long long) next_cursor);
	twitter_http(ic, TWITTER_FRIENDS_IDS_URL, twitter_http_get_friends_ids, ic, 0, args, 2);

	g_free(args[1]);
}

/**
 * Function to help fill a list.
 */
static xt_status twitter_xt_next_cursor( struct xt_node *node, struct twitter_xml_list *txl )
{
	char *end = NULL;
	
	if( node->text )
		txl->next_cursor = g_ascii_strtoll( node->text, &end, 10 );
	if( end == NULL )
		txl->next_cursor = -1;

	return XT_HANDLED;
}

/**
 * Fill a list of ids.
 */
static xt_status twitter_xt_get_friends_id_list( struct xt_node *node, struct twitter_xml_list *txl )
{
	struct xt_node *child;
	
	// Set the list type.
	txl->type = TXL_ID;

	// The root <statuses> node should hold the list of statuses <status>
	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "id", child->name ) == 0)
		{
			// Add the item to the list.
			txl->list = g_slist_append (txl->list, g_memdup( child->text, child->text_len + 1 ));
		}
		else if ( g_strcasecmp( "next_cursor", child->name ) == 0)
		{
			twitter_xt_next_cursor(child, txl);
		}
	}

	return XT_HANDLED;
}

/**
 * Callback for getting the friends ids.
 */
static void twitter_http_get_friends_ids(struct http_request *req)
{
	struct im_connection *ic;
	struct xt_parser *parser;
	struct twitter_xml_list *txl;
	struct twitter_data *td;

	ic = req->data;

	// Check if the connection is still active.
	if( !g_slist_find( twitter_connections, ic ) )
		return;
	
	td = ic->proto_data;

	// Check if the HTTP request went well.
	if (req->status_code != 200) {
		// It didn't go well, output the error and return.
		if (++td->http_fails >= 5)
			imcb_error(ic, "Could not retrieve friends: %s", twitter_parse_error(req));
		
		return;
	} else {
		td->http_fails = 0;
	}

	txl = g_new0(struct twitter_xml_list, 1);

	// Parse the data.
	parser = xt_new( NULL, txl );
	xt_feed( parser, req->reply_body, req->body_size );
	twitter_xt_get_friends_id_list(parser->root, txl);
	xt_free( parser );

	if (txl->next_cursor)
		twitter_get_friends_ids(ic, txl->next_cursor);

	txl_free(txl);
}

/**
 * Function to fill a twitter_xml_user struct.
 * It sets:
 *  - the name and
 *  - the screen_name.
 */
static xt_status twitter_xt_get_user( struct xt_node *node, struct twitter_xml_user *txu )
{
	struct xt_node *child;

	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "name", child->name ) == 0)
		{
			txu->name = g_memdup( child->text, child->text_len + 1 );
		}
		else if (g_strcasecmp( "screen_name", child->name ) == 0)
		{
			txu->screen_name = g_memdup( child->text, child->text_len + 1 );
		}
	}
	return XT_HANDLED;
}

/**
 * Function to fill a twitter_xml_list struct.
 * It sets:
 *  - all <user>s from the <users> element.
 */
static xt_status twitter_xt_get_users( struct xt_node *node, struct twitter_xml_list *txl )
{
	struct twitter_xml_user *txu;
	struct xt_node *child;

	// Set the type of the list.
	txl->type = TXL_USER;

	// The root <users> node should hold the list of users <user>
	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "user", child->name ) == 0)
		{
			txu = g_new0(struct twitter_xml_user, 1);
			twitter_xt_get_user(child, txu);
			// Put the item in the front of the list.
			txl->list = g_slist_prepend (txl->list, txu);
		}
	}

	return XT_HANDLED;
}

/**
 * Function to fill a twitter_xml_list struct.
 * It calls twitter_xt_get_users to get the <user>s from a <users> element.
 * It sets:
 *  - the next_cursor.
 */
static xt_status twitter_xt_get_user_list( struct xt_node *node, struct twitter_xml_list *txl )
{
	struct xt_node *child;

	// Set the type of the list.
	txl->type = TXL_USER;

	// The root <user_list> node should hold a users <users> element
	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "users", child->name ) == 0)
		{
			twitter_xt_get_users(child, txl);
		}
		else if ( g_strcasecmp( "next_cursor", child->name ) == 0)
		{
			twitter_xt_next_cursor(child, txl);
		}
	}

	return XT_HANDLED;
}

#ifdef __GLIBC__
#define TWITTER_TIME_FORMAT "%a %b %d %H:%M:%S %z %Y"
#else
#define TWITTER_TIME_FORMAT "%a %b %d %H:%M:%S +0000 %Y"
#endif

/**
 * Function to fill a twitter_xml_status struct.
 * It sets:
 *  - the status text and
 *  - the created_at timestamp and
 *  - the status id and
 *  - the user in a twitter_xml_user struct.
 */
static xt_status twitter_xt_get_status( struct xt_node *node, struct twitter_xml_status *txs )
{
	struct xt_node *child, *rt = NULL;
	gboolean truncated = FALSE;

	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "text", child->name ) == 0)
		{
			txs->text = g_memdup( child->text, child->text_len + 1 );
		}
		else if (g_strcasecmp( "truncated", child->name ) == 0 && child->text)
		{
			truncated = bool2int(child->text);
		}
		else if (g_strcasecmp( "retweeted_status", child->name ) == 0)
		{
			rt = child;
		}
		else if (g_strcasecmp( "created_at", child->name ) == 0)
		{
			struct tm parsed;
			
			/* Very sensitive to changes to the formatting of
			   this field. :-( Also assumes the timezone used
			   is UTC since C time handling functions suck. */
			if( strptime( child->text, TWITTER_TIME_FORMAT, &parsed ) != NULL )
				txs->created_at = mktime_utc( &parsed );
		}
		else if (g_strcasecmp( "user", child->name ) == 0)
		{
			txs->user = g_new0(struct twitter_xml_user, 1);
			twitter_xt_get_user( child, txs->user );
		}
		else if (g_strcasecmp( "id", child->name ) == 0)
		{
			txs->id = g_ascii_strtoull (child->text, NULL, 10);
		}
	}
	
	/* If it's a truncated retweet, get the original because dots suck. */
	if (truncated && rt)
	{
		struct twitter_xml_status *rtxs = g_new0(struct twitter_xml_status, 1);
		if (twitter_xt_get_status(rt, rtxs) != XT_HANDLED)
		{
			txs_free(rtxs);
			return XT_HANDLED;
		}
		
		g_free(txs->text);
		txs->text = g_strdup_printf("RT @%s: %s", rtxs->user->screen_name, rtxs->text);
		txs_free(rtxs);
	}
	
	return XT_HANDLED;
}

/**
 * Function to fill a twitter_xml_list struct.
 * It sets:
 *  - all <status>es within the <status> element and
 *  - the next_cursor.
 */
static xt_status twitter_xt_get_status_list( struct im_connection *ic, struct xt_node *node, struct twitter_xml_list *txl )
{
	struct twitter_xml_status *txs;
	struct xt_node *child;
	bee_user_t *bu;

	// Set the type of the list.
	txl->type = TXL_STATUS;

	// The root <statuses> node should hold the list of statuses <status>
	// Walk over the nodes children.
	for( child = node->children ; child ; child = child->next )
	{
		if ( g_strcasecmp( "status", child->name ) == 0)
		{
			txs = g_new0(struct twitter_xml_status, 1);
			twitter_xt_get_status(child, txs);
			// Put the item in the front of the list.
			txl->list = g_slist_prepend (txl->list, txs);
			
			if (txs->user && txs->user->screen_name &&
			    (bu = bee_user_by_handle(ic->bee, ic, txs->user->screen_name)))
			{
				struct twitter_user_data *tud = bu->data;
				
				if (txs->id > tud->last_id)
				{
					tud->last_id = txs->id;
					tud->last_time = txs->created_at;
				}
			}
		}
		else if ( g_strcasecmp( "next_cursor", child->name ) == 0)
		{
			twitter_xt_next_cursor(child, txl);
		}
	}

	return XT_HANDLED;
}

static void twitter_http_get_home_timeline(struct http_request *req);

/**
 * Get the timeline.
 */
void twitter_get_home_timeline(struct im_connection *ic, gint64 next_cursor)
{
	struct twitter_data *td = ic->proto_data;

	char* args[4];
	args[0] = "cursor";
	args[1] = g_strdup_printf ("%lld", (long long) next_cursor);
	if (td->home_timeline_id) {
		args[2] = "since_id";
		args[3] = g_strdup_printf ("%llu", (long long unsigned int) td->home_timeline_id);
	}

	twitter_http(ic, TWITTER_HOME_TIMELINE_URL, twitter_http_get_home_timeline, ic, 0, args, td->home_timeline_id ? 4 : 2);

	g_free(args[1]);
	if (td->home_timeline_id) {
		g_free(args[3]);
	}
}

static void twitter_groupchat_init(struct im_connection *ic)
{
	char *name_hint;
	struct groupchat *gc;
	struct twitter_data *td = ic->proto_data;
	GSList *l;
	
	td->home_timeline_gc = gc = imcb_chat_new( ic, "home/timeline" );
	
	name_hint = g_strdup_printf( "%s_%s", td->prefix, ic->acc->user );
	imcb_chat_name_hint( gc, name_hint );
	g_free( name_hint );
	
	for( l = ic->bee->users; l; l = l->next )
	{
		bee_user_t *bu = l->data;
		if( bu->ic == ic )
			imcb_chat_add_buddy( td->home_timeline_gc, bu->handle );
	}
}

/**
 * Function that is called to see the statuses in a groupchat window.
 */
static void twitter_groupchat(struct im_connection *ic, GSList *list)
{
	struct twitter_data *td = ic->proto_data;
	GSList *l = NULL;
	struct twitter_xml_status *status;
	struct groupchat *gc;

	// Create a new groupchat if it does not exsist.
	if (!td->home_timeline_gc)
		twitter_groupchat_init(ic);
	
	gc = td->home_timeline_gc;
	if (!gc->joined)
		imcb_chat_add_buddy( gc, ic->acc->user );

	for ( l = list; l ; l = g_slist_next(l) )
	{
		status = l->data;
		if (status->user == NULL || status->text == NULL)
			continue;

		twitter_add_buddy(ic, status->user->screen_name, status->user->name);
		
		strip_html(status->text);
		
		// Say it!
		if (g_strcasecmp(td->user, status->user->screen_name) == 0)
			imcb_chat_log (gc, "You: %s", status->text);
		else
			imcb_chat_msg (gc, status->user->screen_name, status->text, 0, status->created_at );
		
		// Update the home_timeline_id to hold the highest id, so that by the next request
		// we won't pick up the updates allready in the list.
		td->home_timeline_id = td->home_timeline_id < status->id ? status->id : td->home_timeline_id;
	}
}

/**
 * Function that is called to see statuses as private messages.
 */
static void twitter_private_message_chat(struct im_connection *ic, GSList *list)
{
	struct twitter_data *td = ic->proto_data;
	GSList *l = NULL;
	struct twitter_xml_status *status;
	char from[MAX_STRING];
	gboolean mode_one;
	
	mode_one = g_strcasecmp( set_getstr( &ic->acc->set, "mode" ), "one" ) == 0;

	if( mode_one )
	{
		g_snprintf( from, sizeof( from ) - 1, "%s_%s", td->prefix, ic->acc->user );
		from[MAX_STRING-1] = '\0';
	}
	
	for ( l = list; l ; l = g_slist_next(l) )
	{
		char *text = NULL;
		
		status = l->data;
		
		strip_html( status->text );
		if( mode_one )
			text = g_strdup_printf( "\002<\002%s\002>\002 %s",
			                        status->user->screen_name, status->text );
		else
			twitter_add_buddy(ic, status->user->screen_name, status->user->name);
		
		imcb_buddy_msg( ic,
		                mode_one ? from : status->user->screen_name,
		                mode_one ? text : status->text,
		                0, status->created_at );
		
		// Update the home_timeline_id to hold the highest id, so that by the next request
		// we won't pick up the updates allready in the list.
		td->home_timeline_id = td->home_timeline_id < status->id ? status->id : td->home_timeline_id;
		
		g_free( text );
	}
}

/**
 * Callback for getting the home timeline.
 */
static void twitter_http_get_home_timeline(struct http_request *req)
{
	struct im_connection *ic = req->data;
	struct twitter_data *td;
	struct xt_parser *parser;
	struct twitter_xml_list *txl;

	// Check if the connection is still active.
	if( !g_slist_find( twitter_connections, ic ) )
		return;
	
	td = ic->proto_data;

	// Check if the HTTP request went well.
	if (req->status_code == 200)
	{
		td->http_fails = 0;
		if (!(ic->flags & OPT_LOGGED_IN))
			imcb_connected(ic);
	}
	else if (req->status_code == 401)
	{
		imcb_error( ic, "Authentication failure" );
		imc_logout( ic, FALSE );
		return;
	}
	else
	{
		// It didn't go well, output the error and return.
		if (++td->http_fails >= 5)
			imcb_error(ic, "Could not retrieve " TWITTER_HOME_TIMELINE_URL ": %s", twitter_parse_error(req));
		
		return;
	}

	txl = g_new0(struct twitter_xml_list, 1);
	txl->list = NULL;

	// Parse the data.
	parser = xt_new( NULL, txl );
	xt_feed( parser, req->reply_body, req->body_size );
	// The root <statuses> node should hold the list of statuses <status>
	twitter_xt_get_status_list(ic, parser->root, txl);
	xt_free( parser );

	// See if the user wants to see the messages in a groupchat window or as private messages.
	if (txl->list == NULL)
		;
	else if (g_strcasecmp(set_getstr(&ic->acc->set, "mode"), "chat") == 0)
		twitter_groupchat(ic, txl->list);
	else
		twitter_private_message_chat(ic, txl->list);

	// Free the structure.	
	txl_free(txl);
}

/**
 * Callback for getting (twitter)friends...
 *
 * Be afraid, be very afraid! This function will potentially add hundreds of "friends". "Who has 
 * hundreds of friends?" you wonder? You probably not, since you are reading the source of 
 * BitlBee... Get a life and meet new people!
 */
static void twitter_http_get_statuses_friends(struct http_request *req)
{
	struct im_connection *ic = req->data;
	struct twitter_data *td;
	struct xt_parser *parser;
	struct twitter_xml_list *txl;
	GSList *l = NULL;
	struct twitter_xml_user *user;

	// Check if the connection is still active.
	if( !g_slist_find( twitter_connections, ic ) )
		return;
	
	td = ic->proto_data;
	
	// Check if the HTTP request went well.
	if (req->status_code == 401)
	{
		imcb_error( ic, "Authentication failure" );
		imc_logout( ic, FALSE );
		return;
	} else if (req->status_code != 200) {
		// It didn't go well, output the error and return.
		imcb_error(ic, "Could not retrieve " TWITTER_SHOW_FRIENDS_URL ": %s", twitter_parse_error(req));
		imc_logout( ic, TRUE );
		return;
	} else {
		td->http_fails = 0;
	}
	
	if( !td->home_timeline_gc &&
	    g_strcasecmp( set_getstr( &ic->acc->set, "mode" ), "chat" ) == 0 )
		twitter_groupchat_init( ic );

	txl = g_new0(struct twitter_xml_list, 1);
	txl->list = NULL;

	// Parse the data.
	parser = xt_new( NULL, txl );
	xt_feed( parser, req->reply_body, req->body_size );

	// Get the user list from the parsed xml feed.
	twitter_xt_get_user_list(parser->root, txl);
	xt_free( parser );

	// Add the users as buddies.
	for ( l = txl->list; l ; l = g_slist_next(l) )
	{
		user = l->data;
		twitter_add_buddy(ic, user->screen_name, user->name);
	}

	// if the next_cursor is set to something bigger then 0 there are more friends to gather.
	if (txl->next_cursor > 0)
	{
		twitter_get_statuses_friends(ic, txl->next_cursor);
	}
	else
	{
		td->flags |= TWITTER_HAVE_FRIENDS;
		twitter_login_finish(ic);
	}
	
	// Free the structure.
	txl_free(txl);
}

/**
 * Get the friends.
 */
void twitter_get_statuses_friends(struct im_connection *ic, gint64 next_cursor)
{
	char* args[2];
	args[0] = "cursor";
	args[1] = g_strdup_printf ("%lld", (long long) next_cursor);

	twitter_http(ic, TWITTER_SHOW_FRIENDS_URL, twitter_http_get_statuses_friends, ic, 0, args, 2);

	g_free(args[1]);
}

/**
 * Callback to use after sending a post request to twitter.
 */
static void twitter_http_post(struct http_request *req)
{
	struct im_connection *ic = req->data;
	struct twitter_data *td;

	// Check if the connection is still active.
	if( !g_slist_find( twitter_connections, ic ) )
		return;

	td = ic->proto_data;
	td->last_status_id = 0;
	
	// Check if the HTTP request went well.
	if (req->status_code != 200) {
		// It didn't go well, output the error and return.
		imcb_error(ic, "HTTP error: %s", twitter_parse_error(req));
		return;
	}
	
	if (req->body_size > 0)
	{
		struct xt_parser *xp = NULL;
		struct xt_node *node;
		
		xp = xt_new(NULL, NULL);
		xt_feed(xp, req->reply_body, req->body_size);
		
		if ((node = xt_find_node(xp->root, "status")) &&
		    (node = xt_find_node(node->children, "id")) && node->text)
			td->last_status_id = g_ascii_strtoull( node->text, NULL, 10 );
		
		xt_free(xp);
	}
}

/**
 * Function to POST a new status to twitter.
 */ 
void twitter_post_status(struct im_connection *ic, char *msg, guint64 in_reply_to)
{
	char* args[4] = {
		"status", msg,
		"in_reply_to_status_id",
		g_strdup_printf("%llu", (unsigned long long) in_reply_to)
	};
	twitter_http(ic, TWITTER_STATUS_UPDATE_URL, twitter_http_post, ic, 1,
	             args, in_reply_to ? 4 : 2);
	g_free(args[3]);
}


/**
 * Function to POST a new message to twitter.
 */
void twitter_direct_messages_new(struct im_connection *ic, char *who, char *msg)
{
	char* args[4];
	args[0] = "screen_name";
	args[1] = who;
	args[2] = "text";
	args[3] = msg;
	// Use the same callback as for twitter_post_status, since it does basically the same.
	twitter_http(ic, TWITTER_DIRECT_MESSAGES_NEW_URL, twitter_http_post, ic, 1, args, 4);
//	g_free(args[1]);
//	g_free(args[3]);
}

void twitter_friendships_create_destroy(struct im_connection *ic, char *who, int create)
{
	char* args[2];
	args[0] = "screen_name";
	args[1] = who;
	twitter_http(ic, create ? TWITTER_FRIENDSHIPS_CREATE_URL : TWITTER_FRIENDSHIPS_DESTROY_URL, twitter_http_post, ic, 1, args, 2);
}

void twitter_status_destroy(struct im_connection *ic, guint64 id)
{
	char *url;
	url = g_strdup_printf("%s%llu%s", TWITTER_STATUS_DESTROY_URL, (unsigned long long) id, ".xml");
	twitter_http(ic, url, twitter_http_post, ic, 1, NULL, 0);
	g_free(url);
}

void twitter_status_retweet(struct im_connection *ic, guint64 id)
{
	char *url;
	url = g_strdup_printf("%s%llu%s", TWITTER_STATUS_RETWEET_URL, (unsigned long long) id, ".xml");
	twitter_http(ic, url, twitter_http_post, ic, 1, NULL, 0);
	g_free(url);
}
