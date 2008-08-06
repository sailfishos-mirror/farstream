/*
 * Farsight2 - Farsight MSN Stream
 *
 *  @author: Richard Spiers <richard.spiers@gmail.com>
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 *  @Rob Taylor, Philippe Khalaf   ?  
 *
 * fs-msn-stream.c - A Farsight MSN Stream gobject
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:fs-msn-stream
 * @short_description: A MSN stream in a #FsMsnSession in a #FsMsnConference
 *

 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-msn-stream.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#include <gst/gst.h>

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_DIRECTION,
  PROP_PARTICIPANT,
  PROP_SESSION,
  PROP_SINK_PAD,
  PROP_SRC_PAD,
  PROP_CONFERENCE
};

struct _FsMsnStreamPrivate
{
  FsMsnSession *session;
  FsMsnParticipant *participant;
  FsStreamDirection direction;
  GArray *fdlist;
  FsMsnConference *conference;
  GstPad *sink_pad,*src_pad;


  /* Protected by the session mutex */

  GError *construction_error;

  gboolean disposed;
};


G_DEFINE_TYPE(FsMsnStream, fs_msn_stream, FS_TYPE_STREAM);

#define FS_MSN_STREAM_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_MSN_STREAM, FsMsnStreamPrivate))

static void fs_msn_stream_dispose (GObject *object);
static void fs_msn_stream_finalize (GObject *object);

static void fs_msn_stream_get_property (GObject *object,
                                    guint prop_id,
                                    GValue *value,
                                    GParamSpec *pspec);
static void fs_msn_stream_set_property (GObject *object,
                                    guint prop_id,
                                    const GValue *value,
                                    GParamSpec *pspec);

static void fs_msn_stream_constructed (GObject *object);

static gboolean fs_msn_stream_set_remote_candidates (FsStream *stream,
                                                     GList *candidates,
                                                     GError **error);

/* Needed ?   
static void _local_candidates_prepared (
    FsStreamTransmitter *stream_transmitter,
    gpointer user_data);
    
static void _new_active_candidate_pair (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate1,
    FsCandidate *candidate2,
    gpointer user_data);
    
static void _new_local_candidate (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate,
    gpointer user_data);
    
*/

static GObjectClass *parent_class = NULL;
static guint signals[LAST_SIGNAL] = { 0 };

static void
fs_msn_stream_class_init (FsMsnStreamClass *klass)
{
  GObjectClass *gobject_class;
  FsStreamClass *stream_class = FS_STREAM_CLASS (klass);

  gobject_class = (GObjectClass *) klass;
  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = fs_msn_stream_set_property;
  gobject_class->get_property = fs_msn_stream_get_property;
  gobject_class->constructed = fs_msn_stream_constructed;
  gobject_class->dispose = fs_msn_stream_dispose;
  gobject_class->finalize = fs_msn_stream_finalize;


  stream_class->set_remote_candidates = fs_msn_stream_set_remote_candidates;


  g_type_class_add_private (klass, sizeof (FsMsnStreamPrivate));

  g_object_class_override_property (gobject_class,
      PROP_DIRECTION,
      "direction");
  g_object_class_override_property (gobject_class,
      PROP_PARTICIPANT,
      "participant");
  g_object_class_override_property (gobject_class,
      PROP_SESSION,
      "session");

  g_object_class_install_property (gobject_class,
      PROP_SINK_PAD,
      g_param_spec_object ("sink-pad",
          "A gstreamer sink pad for this stream",
          "A pad used for sending data on this stream",
          GST_TYPE_PAD,
          G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_SRC_PAD,
      g_param_spec_object ("src-pad",
          "A gstreamer src pad for this stream",
          "A pad used for reading data from this stream",
          GST_TYPE_PAD,
          G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_CONFERENCE,
      g_param_spec_object ("conference",
          "The Conference this stream refers to",
          "This is a conveniance pointer for the Conference",
          FS_TYPE_MSN_CONFERENCE,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));

}

static void
fs_msn_stream_init (FsMsnStream *self)
{
  /* member init */
  self->priv = FS_MSN_STREAM_GET_PRIVATE (self);

  self->priv->disposed = FALSE;
  self->priv->session = NULL;
  self->priv->participant = NULL;

  self->priv->direction = FS_DIRECTION_NONE;
  self->priv->fdlist = g_array_new (FALSE, FALSE, sizeof(GIOChannel *));
}

static void
fs_msn_stream_dispose (GObject *object)
{
  FsMsnStream *self = FS_MSN_STREAM (object);

  if (self->priv->disposed) {
    /* If dispose did already run, return. */
    return;
  }

  if (self->priv->participant) {
    g_object_unref (self->priv->participant);
    self->priv->participant = NULL;
  }

  /* Make sure dispose does not run twice. */
  self->priv->disposed = TRUE;

  if (self->priv->session)
  {
    g_object_unref (self->priv->session);
    self->priv->session = NULL;
  }

  parent_class->dispose (object);
}

static void
fs_msn_stream_finalize (GObject *object)
{
  FsMsnStream *self = FS_MSN_STREAM (object);

  parent_class->finalize (object);
}


static void
fs_msn_stream_get_property (GObject *object,
                            guint prop_id,
                            GValue *value,
                            GParamSpec *pspec)
{
  FsMsnStream *self = FS_MSN_STREAM (object);

  switch (prop_id) {
    case PROP_SESSION:
      g_value_set_object (value, self->priv->session);
      break;
    case PROP_PARTICIPANT:
      g_value_set_object (value, self->priv->participant);
      break;
    case PROP_DIRECTION:
      g_value_set_flags (value, self->priv->direction);
      break;
    case PROP_SINK_PAD:
      g_value_set_object (value, self->priv->sink_pad);
      break;
    case PROP_SRC_PAD:
      g_value_set_object (value, self->priv->src_pad);
      break;
    case PROP_CONFERENCE:
      g_value_set_flags (value, self->priv->conference);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_msn_stream_set_property (GObject *object,
                            guint prop_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
  FsMsnStream *self = FS_MSN_STREAM (object);
  GList *item;

  switch (prop_id) {
    case PROP_SESSION:
      self->priv->session = FS_MSN_SESSION (g_value_dup_object (value));
      break;
    case PROP_PARTICIPANT:
      self->priv->participant = FS_MSN_PARTICIPANT (g_value_dup_object (value));
      break;
    case PROP_DIRECTION:
      self->priv->direction = g_value_get_flags (value);
      break;
    case PROP_CONFERENCE:
      self->priv->conference = FS_MSN_CONFERENCE (g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_msn_stream_constructed (GObject *object)
{

  FsMsnStream *self = FS_MSN_STREAM_CAST (object);

  GstElement *ximagesink;
  ximagesink = gst_element_factory_make("ximagesink","ximagesink");
  gst_bin_add (GST_BIN (self->priv->conference), ximagesink);

  if (self->priv->direction = FS_DIRECTION_SEND)
  {
    GstElement *media_fd_sink;
    GstElement *mimenc;
    GstElement *ffmpegcolorspace;

    ffmpegcolorspace = gst_element_factory_make ("ffmpegcolorspace", "ffmpegcolorspace");

    if (!ffmpegcolorspace)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the ffmpegcolorspace element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), ffmpegcolorspace))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the ffmpegcolorspace element to the FsMsnConference");
      gst_object_unref (ffmpegcolorspace);
      return;
    }

    if (!gst_element_sync_state_with_parent  (ffmpegcolorspace))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for ffmpegcolorspace element");
      return;
    }

    media_fd_sink = gst_element_factory_make ("multifdsink", "send_fd_sink");

    if (!media_fd_sink)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the media_fd_sink element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), media_fd_sink))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the media_fd_sink element to the FsMsnConference");
      gst_object_unref (media_fd_sink);
      return;
    }

    if (!gst_element_sync_state_with_parent  (media_fd_sink))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for media_fd_sink element");
      return;
    }

    mimenc = gst_element_factory_make ("mimenc", "send_mim_enc");

    if (!mimenc)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the mimenc element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), mimenc))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the mimenc element to the FsMsnConference");
      gst_object_unref (mimenc);
      return;
    }

    if (!gst_element_sync_state_with_parent  (mimenc))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for mimenc element");
      return;
    }

    GstPad *tmp_sink_pad = gst_element_get_static_pad (ffmpegcolorspace, "sink");
    self->priv->sink_pad = gst_ghost_pad_new ("sink", tmp_sink_pad);

    gst_element_link_pads (mimenc, "src", ximagesink, "sink");
  }
  else if (self->priv->direction = FS_DIRECTION_RECV)
  {
    GstElement *media_fd_src;
    GstElement *mimdec;
    GstElement *ffmpegcolorspace;

    ffmpegcolorspace = gst_element_factory_make ("ffmpegcolorspace", "ffmpegcolorspace");

    if (!ffmpegcolorspace)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the ffmpegcolorspace element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), ffmpegcolorspace))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the ffmpegcolorspace element to the FsMsnConference");
      gst_object_unref (ffmpegcolorspace);
      return;
    }

    if (!gst_element_sync_state_with_parent  (ffmpegcolorspace))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for ffmpegcolorspace element");
      return;
    }


    media_fd_src = gst_element_factory_make ("fdsrc", "recv_fd_src");

    if (!media_fd_src)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the media_fd_src element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), media_fd_src))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the media_fd_src element to the FsMsnConference");
      gst_object_unref (media_fd_src);
      return;
    }

    if (!gst_element_sync_state_with_parent  (media_fd_src))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for media_fd_src element");
      return;
    }

    mimdec = gst_element_factory_make ("mimdec", "recv_mim_dec");

    if (!mimdec)
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not create the mimdec element");
      return;
    }

    if (!gst_bin_add (GST_BIN (self->priv->conference), mimdec))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not add the mimdec element to the FsMsnConference");
      gst_object_unref (mimdec);
      return;
    }
    if (!gst_element_sync_state_with_parent  (mimdec))
    {
      self->priv->construction_error = g_error_new (FS_ERROR,
          FS_ERROR_CONSTRUCTION,
          "Could not sync state with parent for mimdec element");
      return;
    }


    GstPad *tmp_src_pad = gst_element_get_static_pad (mimdec, "src");
    self->priv->src_pad = gst_ghost_pad_new ("src", tmp_src_pad);
  }

  GST_CALL_PARENT (G_OBJECT_CLASS, constructed, (object));
}

/**
 * fs_msn_stream_set_remote_candidate:
 */
static gboolean
fs_msn_stream_set_remote_candidates (FsStream *stream, GList *candidates,
                                     GError **error)
{
  FsMsnStream *self = FS_MSN_STREAM (stream);

  return FALSE; // FIXME
}

static void fs_msn_open_listening_port (guint16 port,FsMsnStream *self)
{
    g_message ("Attempting to listen on port %d.....\n",port);

  GIOChannel *chan;
  gint fd = -1;
  struct sockaddr_in theiraddr;
  memset(&theiraddr, 0, sizeof(theiraddr));

 if ( (fd = socket(PF_INET, SOCK_STREAM, 0)) == -1 )
 	{
  	// show error
    g_message ("could not create socket!");
    return;
  }

  // set non-blocking mode
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  theiraddr.sin_family = AF_INET;
  theiraddr.sin_port = htons (port);
  // bind
  if (bind(fd, (struct sockaddr *) &theiraddr, sizeof(theiraddr)) != 0)
  	{
  		close (fd);
  		return;
  	}


  /* Listen */
	if (listen(fd, 3) != 0)
		{
    	close (fd);
    	return;
    }
  chan = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (chan, TRUE);

  g_array_append_val (self->priv->fdlist, chan);
  g_message ("Listening on port %d\n",port);
 // FIXME Add a watch to listen out for a connection ?....

}

static gboolean
fs_msn_authenticate_incoming (gint fd, gint recipientid, gint sessionid)
{
    if (fd != 0)
    {
        gchar str[400];
        gchar check[400];

        memset(str, 0, sizeof(str));
        if (recv(fd, str, sizeof(str), 0) != -1)
        {
            g_message ("Got %s, checking if it's auth", str);
            sprintf(str, "recipientid=%d&sessionid=%d\r\n\r\n",
                    recipientid, sessionid);
            if (strcmp (str, check) != 0)
            {
                // send our connected message also
                memset(str, 0, sizeof(str));
                sprintf(str, "connected\r\n\r\n");
                send(fd, str, strlen(str), 0);

                // now we get connected
                memset(str, 0, sizeof(str));
                if (recv(fd, str, sizeof(str), 0) != -1)
                {
                    if (strcmp (str, "connected\r\n\r\n") == 0)
                    {
                        g_message ("Authentication successfull");
                        return TRUE;
                    }
                }
            }
        }
        else
        {
            perror("auth");
        }
    }
    return FALSE;
}

// Authenticate ourselves when connecting out
static gboolean
farsight_msnwebcam_authenticate_outgoing (gint fd, gint recipientid, gint sessionid )
{
    gchar str[400];
    memset(str, 0, sizeof(str));
    if (fd != 0)
    {
        g_message ("Authenticating connection on %d...", fd);
        g_message ("sending : recipientid=%d&sessionid=%d\r\n\r\n", recipientid, sessionid);
        sprintf(str, "recipientid=%d&sessionid=%d\r\n\r\n",
                recipientid, sessionid);
        if (send(fd, str, strlen(str), 0) == -1)
        {
            g_message("sending failed");
            perror("auth");
        }

        memset(str, 0, sizeof(str));
        if (recv(fd, str, sizeof(str), 0) != -1)
        {
            g_message ("Got %s, checking if it's auth", str);
            // we should get a connected message now
            if (strcmp (str, "connected\r\n\r\n") == 0)
            {
                // send our connected message also
                memset(str, 0, sizeof(str));
                sprintf(str, "connected\r\n\r\n");
                send(fd, str, strlen(str), 0);
                g_message ("Authentication successfull");
                return TRUE;
            }
        }
        else
        {
            perror("auth");
        }
    }
    return FALSE;
}


/**
 * fs_msn_stream_new:
 * @session: The #FsMsnSession this stream is a child of
 * @participant: The #FsMsnParticipant this stream is for
 * @direction: the initial #FsDirection for this stream
 * 
 *
 * This function create a new stream
 *
 * Returns: the newly created string or NULL on error
 */

FsMsnStream *
fs_msn_stream_new (FsMsnSession *session,
                   FsMsnParticipant *participant,
                   FsStreamDirection direction,
                   FsMsnConference *conference,
                   GError **error)
{
  FsMsnStream *self = g_object_new (FS_TYPE_MSN_STREAM,
    "session", session,
    "participant", participant,
    "direction", direction,
    "conference",conference,
    NULL);

  if (self->priv->construction_error) {
    g_propagate_error (error, self->priv->construction_error);
    g_object_unref (self);
    return NULL;
  }

  return self;
}