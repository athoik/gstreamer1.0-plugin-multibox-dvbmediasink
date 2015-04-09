#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#if GST_VERSION_MAJOR < 1
#include <gst/gst.h>
#else
#include <gstreamer-1.0/gst/gst.h>
#endif

#include "gstdtsdownmix.h"

static gboolean get_downmix_setting();

static inline gint16 convert(sample_t s)
{
	gint32 i = (gint32)(s * 8388607.5 + 0.5);
	return CLAMP(i, -8388607, 8388607);
}

GST_DEBUG_CATEGORY_STATIC(dtsdownmix_debug);
#define GST_CAT_DEFAULT (dtsdownmix_debug)

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
			"audio/x-dts, framed =(boolean) true; "
			"audio/x-private1-dts, framed =(boolean) true")
	);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_SOMETIMES,
	GST_STATIC_CAPS(
			"audio/x-private1-lpcm, framed =(boolean) true, rate = (int) [ 4000, 96000 ], " "channels = (int) [ 1, 6 ]; "
			)
	);

#if GST_VERSION_MAJOR < 1
static gboolean gst_dtsdownmix_sink_event(GstPad * pad, GstEvent * event);
static GstFlowReturn gst_dtsdownmix_chain(GstPad *pad, GstBuffer *buf);
#else
static gboolean gst_dtsdownmix_sink_event(GstPad * pad, GstObject *parent, GstEvent * event);
static GstFlowReturn gst_dtsdownmix_chain(GstPad *pad, GstObject *parent, GstBuffer *buf);
#endif
static GstStateChangeReturn gst_dtsdownmix_change_state (GstElement * element, GstStateChange transition);

static GstElementClass *parent_class = NULL;

static void gst_dtsdownmix_base_init(gpointer g_class)
{
	GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_factory));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_factory));
#if GST_VERSION_MAJOR < 1
	gst_element_class_set_details_simple(element_class, "DTS audio downmixer",
			"Codec/Decoder/Audio",
			"Downmixes DTS audio streams",
			"PLi team");
#else
	gst_element_class_set_static_metadata(element_class, "DTS audio downmixer",
			"Codec/Decoder/Audio",
			"Downmixes DTS audio streams",
			"PLi team");
#endif
}

static void gst_dtsdownmix_class_init(GstDtsDownmixClass *klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;

	gobject_class = (GObjectClass *) klass;
	gstelement_class = (GstElementClass *) klass;
	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);
	gstelement_class->change_state = gst_dtsdownmix_change_state;
}

static void gst_dtsdownmix_init(GstDtsDownmix *dts, GstDtsDownmixClass * g_class)
{
	GstElement *element = GST_ELEMENT(dts);

	/* create the sink pad */
	dts->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
	gst_pad_set_chain_function (dts->sinkpad,  GST_DEBUG_FUNCPTR (gst_dtsdownmix_chain));
	gst_pad_set_event_function (dts->sinkpad,GST_DEBUG_FUNCPTR (gst_dtsdownmix_sink_event));
	gst_element_add_pad(element, dts->sinkpad);

	gst_segment_init(&dts->segment, GST_FORMAT_UNDEFINED);
}

GType gst_dtsdownmix_get_type(void)
{
	static GType dtsdownmix_type = 0;

	if (!dtsdownmix_type) 
	{
		static const GTypeInfo dtsdownmix_info = 
		{
			sizeof (GstDtsDownmixClass),
			(GBaseInitFunc) gst_dtsdownmix_base_init,
			NULL, (GClassInitFunc) gst_dtsdownmix_class_init,
			NULL,
			NULL,
			sizeof (GstDtsDownmix),
			0,
			(GInstanceInitFunc) gst_dtsdownmix_init,
		};

		dtsdownmix_type =
				g_type_register_static (GST_TYPE_ELEMENT, "GstDtsDownmix", &dtsdownmix_info, 0);

		GST_DEBUG_CATEGORY_INIT(dtsdownmix_debug, "dtsdownmix", 0, "DTS audio downmixer");
	}
	return dtsdownmix_type;
}

#if GST_VERSION_MAJOR < 1
static gboolean gst_dtsdownmix_sink_event(GstPad * pad, GstEvent * event)
#else
static gboolean gst_dtsdownmix_sink_event(GstPad * pad, GstObject *parent, GstEvent * event)
#endif
{
	GstDtsDownmix *dts = GST_DTSDOWNMIX(gst_pad_get_parent(pad));
	gboolean ret = FALSE;

	GST_LOG_OBJECT(dts, "%s event", GST_EVENT_TYPE_NAME(event));

	switch (GST_EVENT_TYPE (event)) 
	{
#if GST_VERSION_MAJOR < 1
		case GST_EVENT_NEWSEGMENT:
		{
			GstFormat format;
			gboolean update;
			gdouble rate;
			gint64 start, end, pos;
			gst_event_parse_new_segment(event, &update, &rate, &format, &start, &end, &pos);
			if (format != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID (start)) 
			{
				GST_WARNING ("No time in newsegment event %p (format is %s)",
						event, gst_format_get_name (format));
				gst_event_unref(event);
				dts->sent_segment = FALSE;
				/* set some dummy values, FIXME: do proper conversion */
				start = pos = 0;
				format = GST_FORMAT_TIME;
				end = -1;
			} 
			else 
			{
				dts->sent_segment = TRUE;
				if (dts->srcpad)
				{
					ret = gst_pad_push_event(dts->srcpad, event);
				}
			}

			gst_segment_set_newsegment(&dts->segment, update, rate, format, start, end, pos);
#else
		case GST_EVENT_CAPS:
			if (!get_downmix_setting())
			{
				ret = FALSE;
			}
			else if (dts->srcpad)
			{
				GstCaps *caps;
				GstCaps *srccaps = gst_caps_from_string("audio/x-private1-lpcm, framed =(boolean) true");
				
				gst_event_parse_caps(event, &caps);
				ret = gst_pad_set_caps(dts->srcpad, srccaps);

				gst_caps_unref(srccaps);
				gst_event_unref(event);
			}
			break;
		case GST_EVENT_SEGMENT:
			gst_event_copy_segment(event, &dts->segment);
			dts->sent_segment = TRUE;
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
#endif
		case GST_EVENT_TAG:
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
		case GST_EVENT_EOS:
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
		case GST_EVENT_FLUSH_START:
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
		case GST_EVENT_FLUSH_STOP:
			if (dts->cache)
			{
				gst_buffer_unref(dts->cache);
				dts->cache = NULL;
			}
			gst_segment_init(&dts->segment, GST_FORMAT_UNDEFINED);
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
		default:
			if (dts->srcpad)
			{
				ret = gst_pad_push_event(dts->srcpad, event);
			}
			break;
	}

	gst_object_unref(dts);
	return ret;
}

static void gst_dtsdownmix_update_streaminfo(GstDtsDownmix *dts)
{
	GstTagList *taglist;

#if GST_VERSION_MAJOR < 1
	taglist = gst_tag_list_new();
	gst_tag_list_add(taglist, GST_TAG_MERGE_APPEND,
			GST_TAG_BITRATE, (guint) dts->bit_rate, NULL);
	gst_element_found_tags_for_pad(GST_ELEMENT(dts), dts->srcpad, taglist);
#else
	taglist = gst_tag_list_new(GST_TAG_BITRATE, (guint) dts->bit_rate, NULL);
	gst_pad_push_event(dts->srcpad, gst_event_new_tag(taglist));
#endif
}

static GstFlowReturn gst_dtsdownmix_handle_frame(GstDtsDownmix *dts, guint8 *data, guint length)
{
	gint num_blocks;
	GstBuffer *buffer = NULL;
	level_t level = 1;
	sample_t bias = 0;
	gint flags = DCA_STEREO; /* force downmix to stereo */

	flags |= DCA_ADJUST_LEVEL;

	/* process */
	if (dca_frame(dts->state, data, &flags, &level, bias))
	{
		GST_WARNING_OBJECT(dts, "dts_frame error");
		return GST_FLOW_ERROR;
	}

	/* handle decoded data, one block is 256 samples */
	num_blocks = dca_blocks_num(dts->state);

#if GST_VERSION_MAJOR < 1
	if (gst_pad_alloc_buffer_and_set_caps(dts->srcpad, 0, num_blocks * 256 * dts->using_channels * 2 + 7, GST_PAD_CAPS(dts->srcpad), &buffer) == GST_FLOW_OK)
#else
	if ((buffer = gst_buffer_new_allocate(NULL, num_blocks * 256 * dts->using_channels * 3 + 7, NULL)))
#endif
	{
		gint i;
		gint8 *dest;
		gint8 *header;
#if GST_VERSION_MAJOR < 1
		header = GST_BUFFER_DATA(buffer);
#else
		GstMapInfo map;
		gst_buffer_map(buffer, &map, GST_MAP_WRITE);
		header = map.data;
#endif
		GST_BUFFER_DURATION(buffer) = num_blocks * GST_SECOND * 256 / dts->sample_rate;
		GST_BUFFER_TIMESTAMP(buffer) = dts->timestamp;
		dts->timestamp += GST_BUFFER_DURATION(buffer);

		gint8 freq_code = 0;

		switch (dts->sample_rate)
		{
		default:
		case 48000:
			freq_code = 0;
			break;
		case 96000:
			freq_code = 1;
			break;
		/* TODO Check if 44100 and 32000 frequency is supported
		case 44100:
			freq_code = 2;
			break;
		case 32000:
			freq_code = 3;
			break; */
		}

		/*
		 * LPCM DVD header :
		 * - number of frames in this packet (8 bits)
		 * - first access unit (16 bits) == 0x0003 ?
		 * - emphasis (1 bit)
		 * - mute (1 bit)
		 * - reserved (1 bit)
		 * - current frame (5 bits)
		 * - quantisation (2 bits) 0 == 16bps, 1 == 20bps, 2 == 24bps, 3 == illegal
		 * - frequency (2 bits) 0 == 48 kHz, 1 == 96 kHz, 2 == 44.1 kHz, 3 == 32 kHz
		 * - reserved (1 bit)
		 * - number of channels - 1 (3 bits) 1 == 2 channels
		 * - dynamic range (8 bits) 0x80 == neutral
		 *
		 */

		*header++ = 0xa0;
		*header++ = 0x02; /* frame count */
		*header++ = 0x00; /* first access unit pointer msb */
		*header++ = 0x04; /* first access unit pointer lsb: skip header */
		*header++ = 0x00; /* frame number */
		//*header++ = (2 << 6) | (freq_code << 4) | (dts->using_channels - 1);
		// 2<<6|0<<4|1 = 128|0|1 = 129 = 0x81
		*header++ = 0x81;
		*header++ = 0x80; /* neutral dynamic range */

		dest = (gint8*)header;

		for (i = 0; i < num_blocks; i++)
		{
			if (dca_block(dts->state))
			{
				GST_WARNING_OBJECT(dts, "dts_block error %d", i);
				dest += 256 * dts->using_channels * 3;
			}
			else
			{
				int n;
				gint32 sample1, sample2, sample3, sample4;
				for (n = 0; n < 256; n+=2)
				{
					sample1 = convert(dts->samples[n]);
					sample2 = convert(dts->samples[256 + n]);
					sample3 = convert(dts->samples[1 + n]);
					sample4 = convert(dts->samples[1 + 256 + n]);

					dest[0] = sample1 >> 16;
					dest[1] = (sample1 >> 8) & 0xff;
					dest[2] = sample2 >> 16;
					dest[3] = (sample2 >> 8) & 0xff;
					dest[4] = sample3 >> 16;
					dest[5] = (sample3 >> 8) & 0xff;
					dest[6] = sample4 >> 16;
					dest[7] = (sample4 >> 8) & 0xff;
					dest[8] = sample1 & 0xff;
					dest[9] = sample2 & 0xff;
					dest[10] = sample3 & 0xff;
					dest[11] = sample4 & 0xff;

					dest += 12;
				}
			}
		}
#if GST_VERSION_MAJOR >= 1
		gst_buffer_unmap(buffer, &map);
#endif
		/* push on */
		return gst_pad_push(dts->srcpad, buffer);
	}
	else
	{
		return GST_FLOW_ERROR;
	}
}

#if GST_VERSION_MAJOR < 1
static GstFlowReturn gst_dtsdownmix_chain(GstPad *pad, GstBuffer *buf)
#else
static GstFlowReturn gst_dtsdownmix_chain(GstPad *pad, GstObject *parent, GstBuffer *buf)
#endif
{
	GstDtsDownmix *dts;
	guint8 *data;
	gsize size;
	gint bit_rate = -1;
#if GST_VERSION_MAJOR >= 1
	GstMapInfo map;
#endif

	dts = GST_DTSDOWNMIX(GST_PAD_PARENT(pad));

	if (!dts->srcpad)
	{
		return GST_FLOW_ERROR;
	}

	if (GST_BUFFER_IS_DISCONT(buf)) 
	{
		GST_LOG("received DISCONT");
		if (dts->cache) 
		{
			gst_buffer_unref(dts->cache);
			dts->cache = NULL;
		}
		dts->timestamp = GST_CLOCK_TIME_NONE;
	}
	if (dts->timestamp == GST_CLOCK_TIME_NONE)
	{
		dts->timestamp = GST_BUFFER_TIMESTAMP(buf);
	}

	if (!dts->sent_segment) 
	{
		GstSegment segment;

		/* Create a basic segment. Usually, we'll get a new-segment sent by 
			* another element that will know more information (a demuxer). If we're
			* just looking at a raw AC3 stream, we won't - so we need to send one
			* here, but we don't know much info, so just send a minimal TIME 
			* new-segment event
			*/
		gst_segment_init(&segment, GST_FORMAT_TIME);
#if GST_VERSION_MAJOR < 1
		gst_pad_push_event(dts->srcpad, gst_event_new_new_segment(FALSE,
						segment.rate, segment.format, segment.start,
						segment.duration, segment.start));
#else
		gst_pad_push_event(dts->srcpad, gst_event_new_segment(&segment));
#endif
		dts->sent_segment = TRUE;
	}

	/* merge with cache, if any */
	if (dts->cache)
	{
#if GST_VERSION_MAJOR < 1
		buf = gst_buffer_join(dts->cache, buf);
#else
		buf = gst_buffer_append(dts->cache, buf);
#endif
		dts->cache = NULL;
	}

#if GST_VERSION_MAJOR < 1
	data = GST_BUFFER_DATA(buf);
	size = GST_BUFFER_SIZE(buf);
#else
	gst_buffer_map(buf, &map, GST_MAP_READ);
	data = map.data;
	size = map.size;
#endif
	while (size >= 7)
	{
		if (dts->dtsheader[0])
		{
			if (memcmp(dts->dtsheader, data, 4))
			{
				data++;
				size--;
				continue;
			}
		}
		else
		{
			/* find and read header */
			gint frame_length;
			gint flags;
			dts->framelength = dca_syncinfo(dts->state, data, &flags, &dts->sample_rate, &bit_rate, &frame_length);
		}
		if (dts->framelength == 0)
		{
			/* shift window to re-find sync */
			data++;
			size--;
		}
		else if (dts->framelength <= size)
		{
			if (!dts->dtsheader[0]) 
			{
				memcpy(dts->dtsheader, data, 4);
			}
			if (bit_rate != dts->bit_rate) 
			{
				dts->bit_rate = bit_rate;
				gst_dtsdownmix_update_streaminfo(dts);
			}
			if (gst_dtsdownmix_handle_frame(dts, data, dts->framelength) != GST_FLOW_OK)
			{
				GST_LOG("No frame found");
				size = 0;
				break;
			}
			size -= dts->framelength;
			data += dts->framelength;
		}
		else
		{
			GST_LOG("Not enough data available (needed %d had %d)", dts->framelength, size);
			break;
		}
	}

	if (size > 0)
	{
		/* keep cache */
		gsize fullsize;
#if GST_VERSION_MAJOR < 1
		fullsize = GST_BUFFER_SIZE(buf);
#else
		fullsize = gst_buffer_get_size(buf);
#endif
#if GST_VERSION_MAJOR < 1
		dts->cache = gst_buffer_create_sub(buf, fullsize - size, size);
#else
		dts->cache = gst_buffer_copy_region(buf, GST_BUFFER_COPY_ALL, fullsize - size, size);
#endif
	}

#if GST_VERSION_MAJOR >= 1
	gst_buffer_unmap(buf, &map);
#endif
	gst_buffer_unref(buf);
	return GST_FLOW_OK;
}

static void set_stcmode(GstDtsDownmix *dts)
{
	FILE *f;
	f = fopen("/proc/stb/stc/0/sync", "r");
	if (f)
	{
		fgets(dts->stcmode, sizeof(dts->stcmode), f);
		fclose(f);
	}
	f = fopen("/proc/stb/stc/0/sync", "w");
	if (f)
	{
		fprintf(f, "audio");
		fclose(f);
	}
}

static void restore_stcmode(GstDtsDownmix *dts)
{
	if (dts->stcmode[0])
	{
		FILE *f = fopen("/proc/stb/stc/0/sync", "w");
		if (f)
		{
			fputs(dts->stcmode, f);
			fclose(f);
		}
	}
}

static gboolean get_downmix_setting()
{
	FILE *f;
	char buffer[32] = {0};
	f = fopen("/proc/stb/audio/ac3", "r");
	if (f)
	{
		fread(buffer, sizeof(buffer), 1, f);
		fclose(f);
	}
	return !strncmp(buffer, "downmix", 7);
}

static GstStateChangeReturn gst_dtsdownmix_change_state(GstElement * element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstDtsDownmix *dts = GST_DTSDOWNMIX(element);

	switch (transition) 
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
			dts->state = NULL;
			dts->srcpad = NULL;
			if (!get_downmix_setting())
			{
				return GST_STATE_CHANGE_FAILURE;
			}
			else
			{
				GstCaps *srccaps = gst_caps_from_string("audio/x-private1-lpcm, framed =(boolean) true");
				GstElementClass *klass = GST_ELEMENT_GET_CLASS(dts);
				GstPadTemplate *templ = gst_element_class_get_pad_template(klass, "src");
				if (dts->srcpad)
				{
					gst_element_remove_pad(GST_ELEMENT(dts), dts->srcpad);
					dts->srcpad = NULL;
				}

				dts->srcpad = gst_pad_new_from_template(templ, "src");

				gst_pad_set_caps(dts->srcpad, srccaps);
				gst_pad_set_active(dts->srcpad, TRUE);
				gst_caps_unref(srccaps);
				gst_element_add_pad(GST_ELEMENT(dts), dts->srcpad);
			}
			dts->stcmode[0] = 0;
			set_stcmode(dts);
			dts->state = dca_init(0);
			dts->bit_rate = -1;
			break;
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			dts->samples = dca_samples(dts->state);
			dts->bit_rate = -1;
			dts->sample_rate = -1;
			dts->using_channels = 2; /* fixed stereo */
			dts->dtsheader[0] = 0;
			dts->timestamp = GST_CLOCK_TIME_NONE;
			dts->sent_segment = FALSE;
			gst_segment_init(&dts->segment, GST_FORMAT_UNDEFINED);
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			break;
		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

	switch (transition) 
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			if (dts->cache)
			{
				gst_buffer_unref(dts->cache);
				dts->cache = NULL;
			}
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			if (dts->state)
			{
				dca_free(dts->state);
				dts->state = NULL;
			}
			if (dts->srcpad)
			{
				gst_element_remove_pad(element, dts->srcpad);
				dts->srcpad = NULL;
			}
			restore_stcmode(dts);
			break;
		default:
			break;
	}

	return ret;
}

static gboolean plugin_init (GstPlugin * plugin)
{
	if (!gst_element_register (plugin, "dtsdownmix", GST_RANK_PRIMARY,
					GST_TYPE_DTSDOWNMIX))
		return FALSE;

	return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
		GST_VERSION_MINOR,
#if GST_VERSION_MAJOR < 1
		"dtsdownmix",
#else
		dtsdownmix,
#endif
		"Downmixes DTS audio streams",
		plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/");
