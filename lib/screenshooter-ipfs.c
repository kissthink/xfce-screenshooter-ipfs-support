/*  $Id$
 *
 *  Copyright © 2009-2010 Sebastian Waisbrot <seppo0010@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 * */


#include "screenshooter-ipfs.h"
#include "screenshooter-job-callbacks.h"
#include <string.h>
#include <stdlib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>


static gboolean          ipfs_upload_job          (ScreenshooterJob  *job,
                                                    GArray            *param_values,
                                                    GError           **error);



static char
*get_image_url(const char* json)
{
	JsonParser *parser;
	JsonNode *root;
	GError *error;
	gchar *ret = NULL;
	parser = json_parser_new ();

	error = NULL;
	json_parser_load_from_data(parser, json, strlen(json), &error);

	if (error)
	{
		g_error_free (error);
		g_object_unref (parser);
		return NULL;
	}

	root = json_parser_get_root (parser);
	if (JSON_NODE_TYPE(root) == JSON_NODE_OBJECT) {
		JsonNode *hash = json_object_get_member(json_node_get_object(root), "Hash");
		ret = json_node_dup_string(hash);
	}
	g_object_unref (parser);
	return ret;
}

static gboolean
ipfs_upload_job (ScreenshooterJob *job, GArray *param_values, GError **error)
{

  const gchar *image_path, *title;
  gchar *online_file_name = NULL;
  const gchar* proxy_uri;
  SoupURI *soup_proxy_uri;
#if DEBUG > 0
  SoupLogger *log;
#endif
  guint status;
  SoupSession *session;
  SoupMessage *msg;
  SoupBuffer *buf;
  GMappedFile *mapping;
  SoupMultipart *mp;

  const gchar *upload_url = "https://api.globalupload.io/transport/add";

  GError *tmp_error = NULL;

  g_return_val_if_fail (SCREENSHOOTER_IS_JOB (job), FALSE);
  g_return_val_if_fail (param_values != NULL, FALSE);
  g_return_val_if_fail (param_values->len == 2, FALSE);
  g_return_val_if_fail ((G_VALUE_HOLDS_STRING (&g_array_index(param_values, GValue, 0))), FALSE);
  g_return_val_if_fail ((G_VALUE_HOLDS_STRING (&g_array_index(param_values, GValue, 1))), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_object_set_data (G_OBJECT (job), "jobtype", "ipfs");
  if (exo_job_set_error_if_cancelled (EXO_JOB (job), error))
    return FALSE;


  image_path = g_value_get_string (&g_array_index (param_values, GValue, 0));
  title = g_value_get_string (&g_array_index (param_values, GValue, 1));

  session = soup_session_new ();
#if DEBUG > 0
  log = soup_logger_new (SOUP_LOGGER_LOG_HEADERS, -1);
  soup_session_add_feature (session, (SoupSessionFeature *)log);
#endif

  /* Set the proxy URI if any */
  proxy_uri = g_getenv ("http_proxy");

  if (proxy_uri != NULL)
    {
      soup_proxy_uri = soup_uri_new (proxy_uri);
      g_object_set (session, "proxy-uri", soup_proxy_uri, NULL);
      soup_uri_free (soup_proxy_uri);
    }

  mapping = g_mapped_file_new(image_path, FALSE, NULL);
  if (!mapping) {
    g_object_unref (session);

    return FALSE;
  }

  mp = soup_multipart_new(SOUP_FORM_MIME_TYPE_MULTIPART);
  buf = soup_buffer_new_with_owner (g_mapped_file_get_contents (mapping),
                                    g_mapped_file_get_length (mapping),
                                    mapping, (GDestroyNotify)g_mapped_file_unref);


  soup_multipart_append_form_string (mp, "name", "keyphrase");
  soup_multipart_append_form_string (mp, "name", "user");
  soup_multipart_append_form_file (mp, "file", image_path, NULL, buf);

  msg = soup_form_request_new_from_multipart (upload_url, mp);

  exo_job_info_message (EXO_JOB (job), _("Upload the screenshot..."));
  status = soup_session_send_message (session, msg);

  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      TRACE ("Error during the POST exchange: %d %s\n",
             status, msg->reason_phrase);

      tmp_error = g_error_new (SOUP_HTTP_ERROR,
                         status,
                         _("An error occurred while transferring the data"
                           " to ipfs."));
      g_propagate_error (error, tmp_error);
      g_object_unref (session);
      g_object_unref (msg);

      return FALSE;
    }
  online_file_name = get_image_url (msg->response_body->data);
  /* returned XML is like <data type="array" success="1" status="200"><id>xxxxxx</id> */

  soup_buffer_free (buf);
  g_object_unref (session);
  g_object_unref (msg);

  screenshooter_job_image_uploaded (job, online_file_name);

  return TRUE;
}


/* Public */



/**
 * screenshooter_upload_to_imgur:
 * @image_path: the local path of the image that should be uploaded to
 * imgur.com.
 *
 * Uploads the image whose path is @image_path
 *
 **/

void screenshooter_upload_to_ipfs   (const gchar  *image_path,
                                      const gchar  *title)
{
  ScreenshooterJob *job;
  GtkWidget *dialog, *label;

  g_return_if_fail (image_path != NULL);

  dialog = create_spinner_dialog(_("IPFS"), &label);

  job = screenshooter_simple_job_launch (ipfs_upload_job, 2,
                                          G_TYPE_STRING, image_path,
                                          G_TYPE_STRING, title);

  /* dismiss the spinner dialog after success or error */
  g_signal_connect_swapped (job, "error", G_CALLBACK (gtk_widget_hide), dialog);
  g_signal_connect_swapped (job, "image-uploaded", G_CALLBACK (gtk_widget_hide), dialog);

  g_signal_connect (job, "ask", G_CALLBACK (cb_ask_for_information), NULL);
  g_signal_connect (job, "image-uploaded", G_CALLBACK (cb_image_ipfs_uploaded), NULL);
  g_signal_connect (job, "error", G_CALLBACK (cb_error), NULL);
  g_signal_connect (job, "finished", G_CALLBACK (cb_finished), dialog);
  g_signal_connect (job, "info-message", G_CALLBACK (cb_update_info), label);

  gtk_dialog_run (GTK_DIALOG (dialog));
}
