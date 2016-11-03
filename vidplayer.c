#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <limits.h>
#include <gtk/gtk.h>
#include <gst/gst.h>

#include <gst/video/videooverlay.h>

#include <gdk/gdk.h>

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1)) 
const int COM_LENGTH = 5;
const int WINDOW_WIDTH = 1280;
const int WINDOW_HEIGHT = 720; 
const uint padding = 1;
const int max_videos = 10; 

typedef struct uri_name
  {
  char urname[100];  // pathname string for videos
  } uri_name;

/* Structure to contain all our information, so we can pass it around */
typedef struct CustomData {
  GstElement *pipe[10];           /* Up to ten pipelines */
  GstElement *sink[10];           /* One sink for each pipeline */    
  uri_name   uri[10];             /* List of files to play and path to them */
  gboolean   dropped_file[10];    /* Flag to indicate if a file has been dropped from display */
  gint       drop_count;          /* Number of files dropped from display  */
  GtkWidget *slider;              /* Slider widget to keep track of current position */
  GtkWidget *streams_list;        /* Text widget to display info about the streams */
  gulong slider_update_signal_id; /* Signal ID for the slider update signal */
       
  GstState state;                 /* Current state of the pipeline */
  gint64 duration;                /* Duration of the clip, in nanoseconds */
  gint64 current;                 /* Current playing position in the clip */  
  gint   numpipes;                /* number of pipes/videos to view */
  gint   toprow;                  /* videos in top row */
  gint   zoomnum;                 /* keep track of which window is zoomed */
  GstStateChangeReturn ret; 
  GstBus *bus[10];
  gint   inotifyFd;
  gchar  in_pathstring[256];
  gchar  out_pathstring[256];
  FILE   *in_comfile;
  FILE   *out_comfile;
} CustomData;

/* Structure to hold seek data information, also to pass around with callbacks */
typedef struct seek_data {
  GtkWidget *button,
	    *label,
	    *seek_entry;
} seek_data;

typedef struct handle_data {
  gint       drop_signal[10];     /* Handles of signals for dropped files */
  gint       add_signal[10];      /* Handles of signals for files to add back */
} handle_data;

/* File and watch information for instructions from beyond */
typedef struct file_watch {
  gint   inotifyFd;
  gchar  pathstring[256];
} file_watch;  

/* Drawing area widget and handle for each video and zoom button*/
typedef struct vid_window {
  gboolean   displayed;
  GtkWidget  *vids;
} vid_window;  

  GtkWidget *main_window;               /* The uppermost window, containing all other windows */
  vid_window vid_win[10];               /* The drawing area where the videos will be shown */
  GtkWidget *main_box;                  /* Box to hold main_hboxex and the controls */
  GtkWidget *main_hbox1,*main_hbox2;    /* Boxes to hold the vid_wins */
  GtkWidget *zoom_box;                  /* box to hold zoom control bars */
  GtkWidget *controls;                  /* Box to hold the buttons and the slider */
  GtkWidget *seek_box;                  /* Box to hold seek input and activate button */
  GtkWidget *play_button, *pause_button, *stop_button, *load_button; /* Buttons */
  vid_window zb[10];                    /* zoom buttons */
  GtkWidget *back_button;               /* back from zoom button   */
  GtkWidget *next_button;               /* next button to zoom on next window */
  GtkWidget *quit_button;               /* button to exit program */
  GtkWidget *duration_time, *duration_label; /* To display duration on seek box */
  GtkWidget *current_time;              /* display current position in video */
  GtkWidget *menubar,*file_option,*add_drop,*file_menu,*quit_menu,*load_option;
  GtkWidget *add_files[10],*drop_files[10];
  GtkWidget *add_drop_menu,*add_opt,*add_opt_menu,*drop_opt,*drop_opt_menu;
  GtkWidget *vsep1,*vsep2,*vsep3;
  gboolean   zoomed = FALSE;
  gboolean   firstpass = TRUE; 
  gboolean   newload = FALSE;
  gboolean   outside_command = FALSE;
  seek_data  seek_func;
  gint       NUMPIPES_DISPLAYED,NUMPIPES,TOPROW,ZOOMNUM;
  gint       handler_id[10];
  handle_data da_data;

static void unzoom_cb (GtkButton *button, CustomData *data);


/* This function is called when the PAUSE button is clicked */
static void pause_cb (GtkButton *button, CustomData *data) {
  gint i;
//  GstClockTime timestamp;

  for (i=0; i < data->numpipes; i++){
      gst_element_set_state (data->pipe[i], GST_STATE_PAUSED);
  }

} // pause_cb
       
/* This function is called periodically to refresh the GUI */
static gboolean refresh_ui (CustomData *data) {
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 current[5] = {-1,-1,-1,-1,-1};
  gint64 current_good = -1;
  gboolean  tag1 = FALSE, tag2 = FALSE, tag3 = FALSE;
  gint   i,pcount;
  char   dur_string[30];
  unsigned int usecs = 15;   

  /* We do not want to update anything unless we are in the PAUSED or PLAYING states */
  if (data->state < GST_STATE_PAUSED)
    return TRUE;
  
  /* The design for this next section used to be different. If a single duration query failed */
  /* then the condition failed and it would output the "Could not query current duration" error. */
  /* This may need to be revisited as the current design is potentially bad. */

  /* If we didn't know it yet, query the stream duration */
  if ((!GST_CLOCK_TIME_IS_VALID (data->duration)) || newload) {
    newload = FALSE;
    for (pcount=0; pcount < data->numpipes; pcount++){
        if (gst_element_query_duration (data->pipe[pcount], fmt, &data->duration)) {
          tag1 = TRUE;
	  break; }
	else {
	  printf("Pos 1 Element %d duration could not be queried.\n", pcount);
	}  
    }
    if (tag1) {
      /* Set the range of the slider to the clip duration, in SECONDS */
      gtk_range_set_range (GTK_RANGE (data->slider), 0, (gdouble)data->duration / GST_SECOND);
      sprintf(dur_string, "%02u:%02u:%02u", GST_TIME_ARGS (data->duration));
      gtk_label_set_text (GTK_LABEL(duration_time),dur_string);
      gtk_widget_show(duration_time);}
    else {
      g_printerr ("Could not query current duration.\n");
    }
  }

  for (pcount=0; pcount < data->numpipes; pcount++){
    if (gst_element_query_position (data->pipe[pcount], fmt, &current[pcount])){
      tag2 = TRUE;
      current_good = current[pcount];
      break;
    }
    else {
      tag3 = TRUE;
      printf("Pos 2 Element %d position could not be queried.\n", pcount);
    } 
  }
  if (tag2) {  
    /* Block the "value-changed" signal, so the slider_cb function is not called
     * (which would trigger a seek the user has not requested) */
    g_signal_handler_block (data->slider, data->slider_update_signal_id);
    /* Set the position of the slider to the current pipeline positoin, in SECONDS */
    gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)current_good / GST_SECOND);
    /* Re-enable the signal */
    g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
  }
  sprintf(dur_string, "%02u:%02u:%02u", GST_TIME_ARGS (current_good));
  gtk_label_set_text (GTK_LABEL(current_time),dur_string);
  gtk_widget_show(current_time);
  if ((tag3) && (current_good > 0)) {
    for (i=0; i < data->numpipes; i++){
      if (vid_win[i].displayed) {
        gst_element_seek_simple (data->pipe[i], GST_FORMAT_TIME, 
           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, current_good);
      }
    }   
  }  
  data->current = current_good;
  return TRUE;
} // refresh_ui
         

/* This function is called when a Zoom button is clicked */
static void zoom_cb (GtkButton *button, CustomData *data) {

  gint i;
  GList *children, *iter;

  if (!(zoomed)) {

    gtk_widget_set_sensitive(GTK_WIDGET(add_drop_menu), FALSE);

    gtk_widget_set_sensitive (GTK_WIDGET(load_button), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET(stop_button), FALSE);

    children = gtk_container_get_children(GTK_CONTAINER(main_hbox1));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(main_hbox1), iter->data);
    }  

    children = gtk_container_get_children(GTK_CONTAINER(main_hbox2));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(main_hbox2), iter->data);
    }  

    children = gtk_container_get_children(GTK_CONTAINER(zoom_box));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
//      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(zoom_box), iter->data);
    }  

    for (i=0; i < max_videos; i++){
      if (button == (GTK_BUTTON(zb[i].vids))){
        ZOOMNUM = i;
	break;
      }	
    } 

    g_signal_handler_disconnect(vid_win[ZOOMNUM].vids, handler_id[ZOOMNUM]);
    gtk_widget_set_tooltip_text(vid_win[ZOOMNUM].vids, "Click to view all videos");
    handler_id[ZOOMNUM] = g_signal_connect (vid_win[ZOOMNUM].vids, "button_press_event", 
                                                  G_CALLBACK (unzoom_cb), data); 
    gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[ZOOMNUM].vids, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (zoom_box), back_button, FALSE, FALSE, 0);

    zoomed = TRUE;
    gtk_widget_show_all (main_window);
  }
} // zoom_cb

/* This function is called when a video is clicked to zoom. */
static void zoom_cb_click (GtkWidget *vid_window, CustomData *data) {

  gint i;
  GList *children, *iter;

  if (!(zoomed)) {

    gtk_widget_set_sensitive(GTK_WIDGET(add_drop_menu), FALSE);

    for (i=0; i < max_videos; i++){
      if (vid_window == vid_win[i].vids){
        ZOOMNUM = i;
        break;
      }
    }
    g_signal_handler_disconnect(vid_win[ZOOMNUM].vids, handler_id[ZOOMNUM]);
    gtk_widget_set_tooltip_text(vid_win[ZOOMNUM].vids, "Click to view all videos");
    handler_id[ZOOMNUM] = g_signal_connect (vid_win[ZOOMNUM].vids, "button_press_event", 
                                                  G_CALLBACK (unzoom_cb), data); 
    
    gtk_widget_set_sensitive (GTK_WIDGET(load_button), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET(stop_button), FALSE);

    children = gtk_container_get_children(GTK_CONTAINER(main_hbox1));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(main_hbox1), iter->data);
    }  

    children = gtk_container_get_children(GTK_CONTAINER(main_hbox2));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(main_hbox2), iter->data);
    }  

    children = gtk_container_get_children(GTK_CONTAINER(zoom_box));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
      g_object_ref_sink(iter->data);
      gtk_container_remove(GTK_CONTAINER(zoom_box), iter->data);
    }  

    gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[ZOOMNUM].vids, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (zoom_box), back_button, FALSE, FALSE, 0);

    zoomed = TRUE;
    gtk_widget_show_all (main_window);
  }
} // zoom_cb_click

static void unzoom_cb (GtkButton *button, CustomData *data){

  gint i,j;
  gchar tip_string[30];

  if (zoomed) {

    gtk_widget_set_sensitive(GTK_WIDGET(add_drop_menu), TRUE);

    g_signal_handler_disconnect(vid_win[ZOOMNUM].vids, handler_id[ZOOMNUM]);
    sprintf(tip_string, "Click to enlarge video %i", ZOOMNUM+1);
    gtk_widget_set_tooltip_text(vid_win[ZOOMNUM].vids, tip_string);
    handler_id[ZOOMNUM] = g_signal_connect (vid_win[ZOOMNUM].vids, "button_press_event", 
                                                  G_CALLBACK (zoom_cb_click), data); 
    gtk_container_remove (GTK_CONTAINER(zoom_box), back_button);
    gtk_container_remove (GTK_CONTAINER(main_hbox1), vid_win[ZOOMNUM].vids);

    if (TOPROW == 0){
      for (i = 0; i < max_videos; i++) {
        if (vid_win[i].displayed) {
	  gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[i].vids, TRUE, TRUE, 0);
	  break;
	}  
      }
    }  
    else {
      i = 0;
      j = 0;
      while (i < TOPROW) {
        if (vid_win[j].displayed) {
	  gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[j].vids, TRUE, TRUE, padding);
	  i++;
	  j++;
        }
	else {
	  j++;
	}  
      }
      while (i < NUMPIPES_DISPLAYED) {
        if (vid_win[j].displayed) {
          gtk_box_pack_start (GTK_BOX (main_hbox2), vid_win[j].vids, TRUE, TRUE, padding);
	  i++;
	  j++;
        }
	else {
	  j++;
	}  
      }
    }  

    for (i=0; i < NUMPIPES; i++){
      gtk_box_pack_start (GTK_BOX (zoom_box), (GTK_WIDGET(zb[i].vids)), FALSE, FALSE, padding);
    }
    gtk_widget_set_sensitive (GTK_WIDGET(load_button), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET(stop_button), TRUE);
    gtk_widget_show_all (main_window);
    zoomed = FALSE;
  }
} // unzoom

/* This function is called when the PLAY button is clicked */
static void play_cb (GtkButton *button, CustomData *data) {
  gint i;
  for (i=0; i< data->numpipes; i++) {
      gst_element_set_state (data->pipe[i], GST_STATE_PLAYING);
      gtk_widget_set_sensitive (GTK_WIDGET(zb[i].vids), TRUE);
//    }  
  }
} // play_cb
          
	       
/* This function is called when the STOP button is clicked */
static void stop_cb (GtkButton *button, CustomData *data) {
  gint i;
  for (i=0; i< data->numpipes; i++) {
    gst_element_set_state (data->pipe[i], GST_STATE_READY);
    gtk_widget_set_sensitive (GTK_WIDGET(zb[i].vids), FALSE);
  }
} // stop_cb
    
/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  stop_cb (NULL, data);
  g_signal_connect (widget, "destroy", G_CALLBACK (gtk_main_quit), NULL);
  gtk_main_quit ();
} // delete_event
   
    
/* This function is called when the slider changes its position. We perform a seek to the
 * new position here. */
static void slider_cb (GtkRange *range, CustomData *data) {
  gint i;
  GtkButton  *temp_button;
  gdouble value = gtk_range_get_value (GTK_RANGE (data->slider));
  for (i=0; i < data->numpipes; i++){
    gst_element_seek_simple (data->pipe[i], GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
      (gint64)(value * GST_SECOND));
  }    
} // slider_cb
        
	
/* This function is called when new metadata is discovered in the stream */
static void tags_cb (GstElement *pipe, gint stream, CustomData *data) {
  /* We are possibly in a GStreamer working thread, so we notify the main
   * thread of this event through a message in the bus */
  gst_element_post_message (pipe,
    gst_message_new_application (GST_OBJECT (pipe),
      gst_structure_new ("tags-changed", NULL)));
} // tags_cb

/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;
  gint i;
         
  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);
       
  /* Set the pipeline to READY (which stops playback) */
  for (i=0; i < data->numpipes; i++){
    gst_element_set_state (data->pipe[i], GST_STATE_READY);
  }  
} // error_cb
       
/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  gint i;
  printf ("End-Of-Stream reached.\n");
  for (i=0; i < data->numpipes; i++){
    gst_element_set_state (data->pipe[i], GST_STATE_READY);
  }  
} // eos_cb

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipe[0])) {
    data->state = new_state;
//  printf ("State set to %s\n", gst_element_state_get_name (new_state));
    if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
      /* For extra responsiveness, we refresh the GUI as soon as we reach the PAUSED state */
      refresh_ui (data);
    }
  }
} // state_changed

// **************Originally had below function only for the video pipeline
// *** We are not concerned with the audio and subtitles
// For our purposes, the audio and subtitles were always commented out.
// I was getting text_buffer errors so I commented it out. This may still be usefull
// if we need to analyze any metadata from the video stream
//
/* Extract metadata from all the streams and write it to the text widget in the GUI */
static void analyze_streams (CustomData *data) {
  gint i;
  GstTagList *tags;
  gchar *str, *total_str;
  guint rate;
  gint n_video;  // n_audio, n_text; disregarding audio and subtitle for now
  GtkTextBuffer *text;
    
  /* Clean current contents of the widget */
  text = gtk_text_view_get_buffer (GTK_TEXT_VIEW (data->streams_list));
  gtk_text_buffer_set_text (text, "", -1);
     
  /* Read some properties */
  g_object_get (data->pipe[0], "n-video", &n_video, NULL);
        
  for (i = 0; i < n_video; i++) {
    tags = NULL;
    /* Retrieve the stream's video tags */
    g_signal_emit_by_name (data->pipe[i], "get-video-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("video stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      gst_tag_list_get_string (tags, GST_TAG_VIDEO_CODEC, &str);
      total_str = g_strdup_printf ("  codec: %s\n", str ? str : "unknown");
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      g_free (str);
      gst_tag_list_free (tags);
    }
  }
						         
} // analyze_streams
     
/* This function is called when an "application" message is posted on the bus.
 * Here we retrieve the message posted by the tags_cb callback */
static void application_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  if (g_strcmp0 (gst_message_type_get_name (GST_MESSAGE_TYPE(msg)), "tags-changed") == 0) {
    if (gst_message_has_name (msg, "tags-changed")) {
    /* If the message is the "tags-changed" (only one we are currently issuing), update
     * the stream info GUI */
      analyze_streams (data);
    }
  }
}  

/* This function is called when the seek button has been pressed. */
/* This will seek ahead in the video the number of seconds input  */
/* on the graphical user interface.                               */
void seek_cb (GtkWidget *widget, CustomData *data) {
  gint  seek_value,i;
  GstStateChangeReturn state_return_value;
  GtkWidget *dialog;   
  pause_cb (GTK_BUTTON(pause_button), data);
  for (i=0; i < data->numpipes; i++){
    state_return_value = gst_element_get_state (data->pipe[i], NULL, NULL, -1);
    if (state_return_value == GST_STATE_CHANGE_FAILURE)
     { printf ("Returned value is failure...\n"); 
       return; }
  }   
  const gchar *str = gtk_entry_get_text(GTK_ENTRY(seek_func.seek_entry));
  seek_value = atoi(str);
  if ((data->current + (seek_value * GST_SECOND)) > 
       (data->duration)) {
    dialog = gtk_message_dialog_new (NULL,
                                     GTK_DIALOG_MODAL,
				     GTK_MESSAGE_INFO,
				     GTK_BUTTONS_CLOSE,
				     "Offset out of range");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);			     
    }
  else
    {
    for (i=0; i < data->numpipes; i++){
      gst_element_seek_simple (data->pipe[i], GST_FORMAT_TIME, 
         GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, data->current + (gint64)(seek_value * GST_SECOND));}
    }
  gtk_entry_set_text (GTK_ENTRY(seek_func.seek_entry),"");
  gtk_widget_show(seek_func.seek_entry);
} // seek_cb 

static void redraw_video_gui (gboolean addflag, gint vidnum, CustomData *data) {

  GtkButton  *temp_button;
  gchar tip_string[30];
  gint i,j;
  GList *children, *iter;
  children = gtk_container_get_children(GTK_CONTAINER(main_hbox1));
  for (iter = children; iter != NULL; iter = g_list_next(iter)) {
    g_object_ref_sink(iter->data);
    gtk_container_remove(GTK_CONTAINER(main_hbox1), iter->data);
  }  

  children = gtk_container_get_children(GTK_CONTAINER(main_hbox2));
  for (iter = children; iter != NULL; iter = g_list_next(iter)) {
    g_object_ref_sink(iter->data);
    gtk_container_remove(GTK_CONTAINER(main_hbox2), iter->data);
  }  

  children = gtk_container_get_children(GTK_CONTAINER(zoom_box));
  for (iter = children; iter != NULL; iter = g_list_next(iter)) {
    if (iter->data == zb[vidnum].vids) {
      if (addflag) {
        sprintf(tip_string, "Click to enlarge video %i", vidnum+1);
	gtk_widget_set_tooltip_text(zb[vidnum].vids, tip_string);
	gtk_widget_set_sensitive(GTK_WIDGET(zb[vidnum].vids), TRUE); 
	break;
      }
      else
      {
        sprintf(tip_string, "Dropped video cannot enlarge"); 
	gtk_widget_set_tooltip_text(zb[vidnum].vids, tip_string);
	gtk_widget_set_sensitive(GTK_WIDGET(zb[vidnum].vids), FALSE); 
        break; 
      }
    }
  }  

  TOPROW = NUMPIPES_DISPLAYED/2;

  i = 0;
  j = 0;
  while (i < TOPROW) {
    if (vid_win[j].displayed) {
      gtk_box_pack_start(GTK_BOX(main_hbox1), vid_win[j].vids, TRUE, TRUE, padding);
      i++;
      j++;
    }
    else {
      j++;
    }
  } 
  while (i < NUMPIPES_DISPLAYED) {
    if (vid_win[j].displayed) {
      gtk_box_pack_start(GTK_BOX(main_hbox2), vid_win[j].vids, TRUE, TRUE, padding);
      i++;
      j++;
    }
    else {
      j++;
    }  
  }

} // redraw_video_gui

static void drop_vid_func (GtkMenuItem *drop_vid, CustomData *data)
{ /* Function to remove one of the videos currently displayed */
//  const char * vid_label;
  GtkButton  *temp_button;  
  gint i;

  for (i = 0; i < data->numpipes; i++) {
    if (GTK_WIDGET(drop_vid) == GTK_WIDGET(drop_files[i])) {
      data->dropped_file[i] = TRUE;
      data->drop_count++;
//      printf("Drop file %i and drop_count %i.\n", i+1, data->drop_count); 
      break;
    }
  }

  zb[i].displayed = FALSE;
  vid_win[i].displayed = FALSE;
  NUMPIPES_DISPLAYED--;  
  gtk_widget_set_sensitive (GTK_WIDGET(drop_files[i]), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET(add_files[i]), TRUE);
  if (outside_command) {
    outside_command = FALSE;
  }
  else
  {
//    printf("Writing drop %i command to mini-menu program.\n", i);
    fprintf(data->out_comfile,"drop  %i", i);
    fflush(data->out_comfile);
  }  
//  printf ("Drop count is %i.\n", data->drop_count);
  redraw_video_gui(FALSE, i, data);

} // drop_vid_func

static void add_vid_func (GtkMenuItem *add_vid, CustomData *data)
{ /* Function to restore a video that was previously removed */
  gint i;

  for (i = 0; i < data->numpipes; i++) {
    if (GTK_WIDGET(add_vid) == GTK_WIDGET(add_files[i])) {  
      data->dropped_file[i] = FALSE;
      data->drop_count--;
      break;
    }
  }
    
  zb[i].displayed = TRUE;
  vid_win[i].displayed = TRUE;
  NUMPIPES_DISPLAYED++;  
  gtk_widget_set_sensitive (GTK_WIDGET(drop_files[i]), TRUE);
  gtk_widget_set_sensitive (GTK_WIDGET(add_files[i]), FALSE);
  if (outside_command) {
    outside_command = FALSE;
  }
  else
  {
//    printf("Writing add %i command to mini-menu program.\n", i);
    fprintf(data->out_comfile,"add   %i", i);
    fflush(data->out_comfile);
  }  
  redraw_video_gui(TRUE, i, data);

} // add_vid_func

static void
load_files (GtkButton * button, CustomData *data)
{
  gchar loc_uri[100], tmp_uri[100];
  struct stat st = {0};
  struct dirent *dp;
  DIR           *dirp;
  GtkWidget     *dialog, *load_dialog;
  GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
  GtkButton     *fakebutton;
  gint res, i, j, k;
  gchar video_loc[100];
  gchar duration_string[30], tip_string[30];
  const char* vidnum[] = {"Video1", "Video2", "Video3", "Video4", "Video5",
                          "Video6", "Video7", "Video8", "Video9", "Video10" };
  gchar file_ext[] = "mkv";

  if (zoomed) {
    unzoom_cb ( fakebutton, data );
    zoomed = FALSE;
  }  
  dialog = gtk_file_chooser_dialog_new ("Select Folder",
            NULL,
	    action,
	    "Cancel",
	    GTK_RESPONSE_CANCEL,
	    "_Open", GTK_RESPONSE_ACCEPT, NULL);

  res = gtk_dialog_run (GTK_DIALOG (dialog));
  if (res == GTK_RESPONSE_ACCEPT) {
    char *foldername;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
    foldername = gtk_file_chooser_get_current_folder (chooser);
    strcpy (video_loc, foldername);
//    printf ("Folder chosen : %s\n", video_loc);
    g_free (foldername);
    }
  else
    {
    if (firstpass) {
      // If no files are selected when the program starts, exit gracefully 
        exit (0);
      }
    else
      {  
      gtk_widget_destroy (dialog);
      return;
      }
    }
  gtk_widget_destroy (dialog);

  if (NULL == (dirp = opendir (video_loc))) {
    printf ("Failed to open directory.\n");
  }

  i = 0; 

  strcat(video_loc, "/");
  while (((dp = readdir(dirp)) != NULL) && (i < max_videos)) {

    if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
      continue;
    if (dp->d_type != DT_DIR) {
      
        strcpy(loc_uri, "file://");
        strcat(loc_uri, video_loc);
        strcpy(tmp_uri, video_loc);
        strcat(loc_uri, dp->d_name);
        strcat(tmp_uri, dp->d_name);
        strcpy(data->uri[i].urname, loc_uri);
        if (stat(tmp_uri,&st) == -1){
          printf("Could not get status of %s\n", tmp_uri);
       	}
        i++;
    }
  }

  closedir(dirp);

  // Sort the names; otherwise the videos will appear random
  for (j=0; j < i-1; j++){
    for (k=j+1; k < i; k++){
      if(strcmp(data->uri[j].urname,data->uri[k].urname) > 0){
        strcpy(loc_uri,data->uri[j].urname);
	strcpy(data->uri[j].urname,data->uri[k].urname);
	strcpy(data->uri[k].urname, loc_uri);
      }
    } 
  }

  if (!firstpass) {
    newload = TRUE;
    g_object_ref_sink (seek_box);   
    gtk_container_remove (GTK_CONTAINER(main_box), seek_box);  
    g_object_ref_sink (zoom_box);
    gtk_container_remove (GTK_CONTAINER(main_box), zoom_box);   
    g_object_ref_sink (controls);
    gtk_container_remove (GTK_CONTAINER(main_box), controls);
    g_object_ref_sink (main_box);
    gtk_container_remove (GTK_CONTAINER(main_window), main_box);
    gtk_container_remove (GTK_CONTAINER(main_box), menubar);
    for (j=0; j<data->numpipes; j++) 
      {
      gst_element_set_state (data->pipe[j], GST_STATE_NULL);
      gst_object_unref (data->pipe[j]);
      gst_object_unref (data->bus[j]);
      gtk_widget_destroy (zb[j].vids);
      g_object_unref (zb[j].vids);
      zb[j].displayed = FALSE;
      }
  }

  data->numpipes = i;
  data->toprow = i/2;
//  printf ("Numpipes : %i.\n", data->numpipes);
//  printf ("Top row number : %i.\n", data->toprow);
  NUMPIPES = i;
  NUMPIPES_DISPLAYED = i;
  TOPROW = NUMPIPES_DISPLAYED/2;

/* Create the elements */

  for (i=0; i<data->numpipes; i++)
  {
  data->pipe[i] = gst_element_factory_make ("playbin", "play");
  data->sink[i] = gst_element_factory_make ("gtksink", "sink");
  if ((!data->pipe[i]) || (!data->sink[i])){
    g_printerr ("Not all elements could be created.\n");
    return;
    }
  }
     
//  /* Set the URI to play */

  for (i=0; i<data->numpipes; i++) 
    {
    g_object_set(G_OBJECT(data->pipe[i]), "uri", data->uri[i].urname, NULL);
    g_signal_connect (G_OBJECT (data->pipe[i]), "video-tags-changed", (GCallback) tags_cb, &data);
    g_object_get (data->sink[i], "widget", &vid_win[i].vids, NULL);
    g_object_set (data->pipe[i], "video_sink", data->sink[i], NULL);
    gtk_widget_set_double_buffered (vid_win[i].vids, FALSE);
    vid_win[i].displayed = TRUE;
    sprintf(tip_string, "Click to enlarge video %i", i+1);
    gtk_widget_set_tooltip_text(vid_win[i].vids, tip_string);
    handler_id[i] = g_signal_connect (vid_win[i].vids, "button_press_event", G_CALLBACK (zoom_cb_click), data); 

    /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
    data->bus[i] = gst_element_get_bus (data->pipe[i]);
    gst_bus_add_signal_watch (data->bus[i]);
    g_signal_connect (G_OBJECT (data->bus[i]), "message::error", (GCallback)error_cb, data);
    g_signal_connect (G_OBJECT (data->bus[i]), "message::eos", (GCallback)eos_cb, data);
    g_signal_connect (G_OBJECT (data->bus[i]), "message::state-changed", (GCallback)state_changed_cb, data);
//    All these calls had a parameter of &data. The "&" is not necessary. It was needed when this was 
//    originally called from the main part of the main program.
    g_signal_connect (G_OBJECT (data->bus[i]), "message::application", (GCallback)application_cb, data);
    data->ret = gst_element_set_state (data->pipe[i], GST_STATE_READY);
    if (data->ret == GST_STATE_CHANGE_FAILURE){
      g_printerr ("Unable to set the pipeline to the playing state.\n");
      gst_object_unref (data->pipe[i]);
      return;
      }
    }

 
  if (firstpass) {
    main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width(GTK_CONTAINER(main_window),0);
    g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);
    play_button = gtk_button_new_with_label ("Play");
    gtk_widget_set_tooltip_text (play_button, "Click to start video playback");
    g_signal_connect (G_OBJECT (play_button), "clicked", G_CALLBACK (play_cb), data);
         
    pause_button = gtk_button_new_with_label ("Pause");
    gtk_widget_set_tooltip_text (pause_button, "Click to pause video playback");
    g_signal_connect (G_OBJECT (pause_button), "clicked", G_CALLBACK (pause_cb), data);
        
    stop_button = gtk_button_new_with_label ("Stop");
    gtk_widget_set_tooltip_text (stop_button, "Click to stop video playback");
    g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (stop_cb), data);
       
    load_button = gtk_button_new_with_label ("Load");
    gtk_widget_set_tooltip_text (load_button, "Click to load videos");
    g_signal_connect (G_OBJECT (load_button), "clicked", G_CALLBACK (load_files), data);

    data->slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value (GTK_SCALE (data->slider), 0);
    gtk_widget_set_tooltip_text (data->slider, "Drag to advance video playback location");
    data->slider_update_signal_id = g_signal_connect (G_OBJECT (data->slider), "value-changed", 
        G_CALLBACK (slider_cb), data);
    zoom_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    seek_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (controls), play_button, FALSE, FALSE, 2);
    gtk_box_pack_start (GTK_BOX (controls), pause_button, FALSE, FALSE, 2);
    gtk_box_pack_start (GTK_BOX (controls), stop_button, FALSE, FALSE, 2);
    gtk_box_pack_start (GTK_BOX (controls), load_button, FALSE, FALSE, 2);
    gtk_box_pack_end (GTK_BOX (controls), data->slider, TRUE, TRUE, 2);

    seek_func.label = gtk_label_new("Enter offset time to seek in seconds");
    gtk_box_pack_start(GTK_BOX(seek_box), seek_func.label, FALSE, TRUE, 5);
    seek_func.seek_entry = gtk_entry_new();
    gtk_widget_set_size_request(seek_func.seek_entry, 120, -1);
    gtk_box_pack_start(GTK_BOX(seek_box), seek_func.seek_entry, FALSE, TRUE, 5); 
    seek_func.button = gtk_button_new_with_label ("Seek");
    gtk_box_pack_start(GTK_BOX (seek_box), seek_func.button, FALSE, FALSE, 2);
    current_time = gtk_label_new("          ");
    gtk_box_pack_start(GTK_BOX (seek_box), current_time, FALSE, FALSE, 5);
    duration_time = gtk_label_new("          ");
    gtk_box_pack_start(GTK_BOX (seek_box), duration_time, FALSE, FALSE, 5);
    duration_label = gtk_label_new("Current offset/Video duration");
    gtk_box_pack_start(GTK_BOX (seek_box), duration_label, FALSE, FALSE, 5);
    quit_button = gtk_button_new_with_label ("Quit");
    gtk_box_pack_end(GTK_BOX (seek_box), quit_button, FALSE, FALSE, 2);
    g_signal_connect (G_OBJECT (seek_func.button), "clicked", G_CALLBACK (seek_cb), data);
    g_signal_connect (G_OBJECT (quit_button), "clicked", G_CALLBACK (gtk_main_quit), 
      G_OBJECT (main_window));

    back_button = gtk_button_new_with_label ("Back");
    g_object_ref_sink(back_button);
    g_signal_connect (G_OBJECT (back_button), "clicked", G_CALLBACK (unzoom_cb), data);

    }

  for (i=0; i<data->numpipes; i++) { 
    zb[i].vids = gtk_button_new_with_label (vidnum[i]);
    zb[i].displayed = TRUE;
    g_object_ref_sink(zb[i].vids);
    g_signal_connect (G_OBJECT (zb[i].vids), "clicked", G_CALLBACK (zoom_cb), data);
    sprintf(tip_string, "Click to enlarge video %i", i+1);
    gtk_widget_set_tooltip_text(zb[i].vids, tip_string);
    gtk_widget_set_sensitive (GTK_WIDGET(zb[i].vids), FALSE);
    gtk_box_pack_start (GTK_BOX (zoom_box), zb[i].vids, FALSE, FALSE, 0);
  }

  main_hbox1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  g_object_ref_sink(main_hbox1);
  if (data->numpipes != 0) { 
    if (data->toprow == 0) {
      gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[0].vids, TRUE, TRUE, padding);
      }
    else {
      for (i=0; i < data->toprow; i++) {
        gtk_box_pack_start (GTK_BOX (main_hbox1), vid_win[i].vids, TRUE, TRUE, padding);
        }
    }
  }  
  main_hbox2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  g_object_ref_sink(main_hbox2);
  if (data->numpipes != 0) { 
    if (data->toprow != 0) {
      for (i=data->toprow; i < data->numpipes; i++) {
        gtk_box_pack_start (GTK_BOX (main_hbox2), vid_win[i].vids, TRUE, TRUE, padding);
        }
    }
  }

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  menubar = gtk_menu_bar_new();

  file_menu = gtk_menu_new();
  file_option = gtk_menu_item_new_with_label("File");
  quit_menu = gtk_menu_item_new_with_label("Quit");
  load_option = gtk_menu_item_new_with_label("Load");
  
  add_drop_menu = gtk_menu_new();
  add_drop = gtk_menu_item_new_with_label("Add/Drop");
  add_opt_menu = gtk_menu_new();
  add_opt = gtk_menu_item_new_with_label("Add");
  drop_opt_menu = gtk_menu_new();
  drop_opt = gtk_menu_item_new_with_label("Drop");

  vsep1 = gtk_separator_menu_item_new();
  vsep2 = gtk_separator_menu_item_new();
  vsep3 = gtk_separator_menu_item_new();

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(add_drop), add_drop_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(add_drop_menu), add_opt);
  gtk_menu_shell_append(GTK_MENU_SHELL(add_drop_menu), vsep2);
  gtk_menu_shell_append(GTK_MENU_SHELL(add_drop_menu), drop_opt);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(drop_opt), drop_opt_menu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(add_opt), add_opt_menu);

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_option), file_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), load_option);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), vsep1);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_menu);

  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_option);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), vsep3);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), add_drop);

  g_signal_connect(G_OBJECT(quit_menu), "activate", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(G_OBJECT(load_option), "activate", G_CALLBACK(load_files), data);

  gtk_box_pack_start(GTK_BOX(main_box), menubar, FALSE, FALSE, 0);

//printf("After menu creation.\n");

  for (i=0; i < data->numpipes; i++){
    sprintf(tip_string, "Video %i", i+1);
    add_files[i] = gtk_menu_item_new_with_label(tip_string);
    gtk_menu_shell_append(GTK_MENU_SHELL(add_opt_menu),add_files[i]);
    g_signal_connect(G_OBJECT(add_files[i]), "activate", G_CALLBACK(add_vid_func), data);
    gtk_widget_set_sensitive (GTK_WIDGET(add_files[i]), FALSE);
  }
    
  for (i=0; i < data->numpipes; i++){
    sprintf(tip_string, "Video %i", i+1);
    drop_files[i] = gtk_menu_item_new_with_label(tip_string);
    gtk_menu_shell_append(GTK_MENU_SHELL(drop_opt_menu),drop_files[i]);
    g_signal_connect(G_OBJECT(drop_files[i]), "activate", G_CALLBACK(drop_vid_func), data);
    gtk_widget_set_sensitive (GTK_WIDGET(drop_files[i]), TRUE);
  }

  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox1, TRUE, TRUE, padding);
  gtk_box_pack_end (GTK_BOX (main_box), seek_box, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (main_box), zoom_box, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (main_box), main_hbox2, TRUE, TRUE, padding);
  gtk_window_set_default_size (GTK_WINDOW (main_window), WINDOW_WIDTH, WINDOW_HEIGHT);
  gtk_window_set_title(GTK_WINDOW(main_window), video_loc);    
 
  gtk_widget_show_all (main_window);
  firstpass = FALSE;

  if (data->numpipes == 0) {
    load_dialog = gtk_message_dialog_new (NULL,
                                     GTK_DIALOG_MODAL,
				     GTK_MESSAGE_INFO,
				     GTK_BUTTONS_CLOSE,
				     "Invalid directory selected! Select a directory populated with .mkv video files.");
    gtk_dialog_run(GTK_DIALOG(load_dialog));
    gtk_widget_destroy(load_dialog);			     

    }
  else {
    /* Momentary start/stop to gather information to display on gui */
    play_cb (GTK_BUTTON(play_button), data);
    pause_cb (GTK_BUTTON(pause_button), data);
    fprintf(data->out_comfile, "load  %i", data->numpipes);
    fflush(data->out_comfile);
    }  
} // create_ui
       
// refresh_ui originally located here.
static void poll_control (CustomData *data) {
  char buf[BUF_LEN], command[COM_LENGTH];
  ssize_t numRead;
  char *p;
  int vid_count;
  struct inotify_event *event;
  GtkButton  *temp_button;
  numRead = read(data->inotifyFd, buf, BUF_LEN);


  if (numRead < 0)
    return;

  /* Process all of the events in buffer returned by read() */

  for (p = buf; p < buf + numRead; ) {
    event = (struct inotify_event *) p;
    if (event->mask & IN_MODIFY) {
      fscanf(data->in_comfile, "%s %d", command, &vid_count);
      printf("Command read in %s\n", command);
      if (!strcmp(command,"\0")) {
        break;
      }
      if (!strcmp(command,"play")) {
        play_cb (temp_button, data);
        printf("Command read in matches play\n");
	break;
      }
      if (!strcmp(command,"load")) {
        load_files (temp_button, data);
        printf("Command read in matches load\n");
        break;
      }
      if (!strcmp(command,"stop")) {
        stop_cb (temp_button, data);
        printf("Command read in matches stop\n");
	break;
      }
      if (!strcmp(command,"pause")) {
        pause_cb (temp_button, data);
        printf("Command read in matches pause\n");
	break;
      }
      if (!strcmp(command,"vid")) {
        outside_command = TRUE;
	zoom_cb (GTK_BUTTON(zb[vid_count].vids), data);
        printf("Command read in matches vid   %i\n", vid_count);
	break;
      }
      if (!strcmp(command,"add")) {
        outside_command = TRUE;
        add_vid_func (GTK_MENU_ITEM(add_files[vid_count]), data);
        printf("Command read in matches vid   %i\n", vid_count);
	break;
      }
      if (!strcmp(command,"drop")) {
        outside_command = TRUE;
        drop_vid_func (GTK_MENU_ITEM(drop_files[vid_count]), data);
        printf("Command read in matches vid   %i\n", vid_count);
	break;
      }
      if (!strcmp(command,"back")) {
       unzoom_cb (temp_button, data);
        printf("Command read in matches back\n");
	break;
      }
      	 
    }

    p += sizeof(struct inotify_event) + event->len;
  }
return;
}


static void enable_factory (const gchar *name, gboolean enable) {
  GstRegistry *registry = NULL;
  GstElementFactory *factory = NULL;
	     
  registry = gst_registry_get ();
  if (!registry) return;
		          
  factory = gst_element_factory_find (name);
  if (!factory) return;
				       
  if (enable) {
    gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), GST_RANK_PRIMARY + 1);
  }
  else {
    gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), GST_RANK_NONE);
  }
								            
  gst_registry_add_feature (registry, GST_PLUGIN_FEATURE (factory));
  return;
}


int main(int argc, char *argv[]) {
  struct stat st = {0};
  CustomData data;
  GtkButton  *temp_button;
  int        mkdir_status, wd;
  char      *homedir;
  gint    i;

  /* Initialize GTK */
  gtk_init (&argc, &argv);
   
  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Initialize our data structure */
  memset (&data, 0, sizeof (data));
  data.duration = GST_CLOCK_TIME_NONE;

/* Create inotify instance for later use */
  data.inotifyFd = inotify_init1(IN_NONBLOCK);
  if (data.inotifyFd ==-1)
    perror("Inotify_init");

  /* Get the file with the commands from beyond */
  homedir = getenv("HOME");
  if (homedir != NULL)
    sprintf(data.in_pathstring,"%s", homedir);
  else
    sprintf(data.in_pathstring,"/home/user");
  strcat(data.in_pathstring,"/cap");
  if (stat(data.in_pathstring, &st) == -1) {
//    printf("Folder does not exist, making folder.\n");
    mkdir_status = mkdir(data.in_pathstring, 0777);
    if (mkdir_status == -1) {
//      printf("Failed to create communication folder.\n");
      exit(0);
    }
  }
  else
  {
//    printf("Folder already exists.\n");
  }
  strcat(data.in_pathstring,"/vidco_message.txt");
  if (stat(data.in_pathstring,&st) == -1) {
//    printf("Video message file does not exist, making file.\n");
    data.in_comfile = fopen(data.in_pathstring, "w+");
    if (data.in_comfile == NULL) {
//      printf("Failed to create message file.\n");
      exit(0);
    }
  }
  else
  {
    data.in_comfile = fopen(data.in_pathstring, "a+");
  }

  /* Get the file to write commands back to set up zoom buttons, drop/add menu */
    homedir = getenv("HOME");
    if (homedir != NULL)
      sprintf(data.out_pathstring,"%s", homedir);
    else
      sprintf(data.out_pathstring,"/home/user");
    strcat(data.out_pathstring,"/cap");
    if (stat(data.out_pathstring, &st) == -1) {
//      printf("Folder does not exist, making folder.\n");
      mkdir_status = mkdir(data.out_pathstring, 0777);
      if (mkdir_status == -1) {
//        printf("Failed to create communication folder.\n");
        exit(0);
      }
    } 
    else
    {
//      printf("Folder already exists.\n");
    }
    strcat(data.out_pathstring,"/com_msg.txt");
    if (stat(data.out_pathstring,&st) == -1) {
//      printf("Video message file does not exist, making file.\n");
      data.out_comfile = fopen(data.out_pathstring, "w+");
      if (data.out_comfile == NULL) {
//        printf("Failed to create message file.\n");
        exit(0);
      }
    } 
    else
    {
      data.out_comfile = fopen(data.out_pathstring, "w+");
    }

    
/* Add the watch function for inotify */
  wd = inotify_add_watch(data.inotifyFd, data.in_pathstring, IN_ALL_EVENTS);
  if (wd == -1)
    perror("inotify_add_watch");

  /* Open the command file to read then flush */
  data.in_comfile = fopen( data.in_pathstring, "r+"); 

  /* The bulk of the work is done here. Files are loaded, number of videos to display */
  /* is determined. Call backs for zooming and loading new videos also established.   */
  load_files (temp_button, &data);
    
  /* Check the file once every second */
  g_timeout_add_seconds (1, (GSourceFunc)poll_control, &data);

  /* Enable vaapidecode */
  enable_factory( "vaapidecode", TRUE );

  /* Register a function that GLib will call every second */
  g_timeout_add_seconds (1, (GSourceFunc)refresh_ui, &data);
  
  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main ();
  
  /* Free resources */

  for (i=0; i<data.numpipes; i++) 
    {
    gst_element_set_state (data.pipe[i], GST_STATE_NULL);
    gst_object_unref (data.pipe[i]);
    }
  return 0;
}
