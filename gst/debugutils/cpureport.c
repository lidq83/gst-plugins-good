/* GStreamer Cpu Report Element
 * Copyright (C) <2010> Zaheer Abbas Merali <zaheerabbas merali org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "cpureport.h"


enum
{
  ARG_0,
};

GstStaticPadTemplate cpu_report_src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

GstStaticPadTemplate cpu_report_sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstFlowReturn gst_cpu_report_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

static gboolean gst_cpu_report_start (GstBaseTransform * trans);
static gboolean gst_cpu_report_stop (GstBaseTransform * trans);

GST_BOILERPLATE (GstCpuReport, gst_cpu_report, GstBaseTransform,
    GST_TYPE_BASE_TRANSFORM);

static void
gst_cpu_report_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_static_pad_template (element_class,
      &cpu_report_sink_template);
  gst_element_class_add_static_pad_template (element_class,
      &cpu_report_src_template);

  gst_element_class_set_details_simple (element_class, "CPU report",
      "Testing",
      "Post cpu usage information every buffer",
      "Zaheer Abbas Merali <zaheerabbas at merali dot org>");
}

static void
gst_cpu_report_finalize (GObject * obj)
{
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_cpu_report_class_init (GstCpuReportClass * g_class)
{
  GstBaseTransformClass *gstbasetrans_class;
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (g_class);
  gstbasetrans_class = GST_BASE_TRANSFORM_CLASS (g_class);

  gobject_class->finalize = gst_cpu_report_finalize;

  gstbasetrans_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_cpu_report_transform_ip);
  gstbasetrans_class->start = GST_DEBUG_FUNCPTR (gst_cpu_report_start);
  gstbasetrans_class->stop = GST_DEBUG_FUNCPTR (gst_cpu_report_stop);
}

static void
gst_cpu_report_init (GstCpuReport * report, GstCpuReportClass * g_class)
{
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (report), TRUE);

}

static GstFlowReturn
gst_cpu_report_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstCpuReport *filter;
  GTimeVal cur_time;
  clock_t cur_cpu_time;
  GstMessage *msg;
  GstStructure *s;
  gint64 time_taken;


  g_get_current_time (&cur_time);
  cur_cpu_time = clock ();

  filter = GST_CPU_REPORT (trans);


  time_taken = GST_TIMEVAL_TO_TIME (cur_time) -
      GST_TIMEVAL_TO_TIME (filter->last_time);

  s = gst_structure_new ("cpu-report", "cpu-time", G_TYPE_DOUBLE,
      ((gdouble) (cur_cpu_time - filter->last_cpu_time)),
      "actual-time", G_TYPE_INT64, time_taken, "buffer-time", G_TYPE_INT64,
      GST_BUFFER_TIMESTAMP (buf), NULL);
  msg = gst_message_new_element (GST_OBJECT_CAST (filter), s);
  gst_element_post_message (GST_ELEMENT_CAST (filter), msg);
  filter->last_time = cur_time;
  filter->last_cpu_time = cur_cpu_time;


  return GST_FLOW_OK;
}

static gboolean
gst_cpu_report_start (GstBaseTransform * trans)
{
  GstCpuReport *filter;

  filter = GST_CPU_REPORT (trans);

  g_get_current_time (&filter->last_time);
  filter->start_time = filter->last_time;
  filter->last_cpu_time = clock ();
  return TRUE;
}

static gboolean
gst_cpu_report_stop (GstBaseTransform * trans)
{
  /* anything we should be doing here? */
  return TRUE;
}
