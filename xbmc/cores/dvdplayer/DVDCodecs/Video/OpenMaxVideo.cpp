/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#include "OpenMaxVideo.h"

#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "windowing/WindowingFactory.h"
#include "DVDVideoCodec.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "ApplicationMessenger.h"
#include "Application.h"
#include "threads/Atomics.h"
#include "guilib/GUIWindowManager.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "settings/DisplaySettings.h"
#include "cores/VideoRenderers/RenderManager.h"

#include "linux/RBP.h"

#define DTS_QUEUE

#ifdef _DEBUG
#define OMX_DEBUG_VERBOSE
#endif

#define CLASSNAME "COpenMaxVideoBuffer"

COpenMaxVideoBuffer::COpenMaxVideoBuffer(COpenMaxVideo *omv)
    : m_omv(omv), m_refs(0)
{
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  mmal_buffer = NULL;
  width = 0;
  height = 0;
  index = 0;
  m_aspect_ratio = 0.0f;
  m_changed_count = 0;
  dts = DVD_NOPTS_VALUE;
  m_es_format = mmal_format_alloc();
}

COpenMaxVideoBuffer::~COpenMaxVideoBuffer()
{
  mmal_format_free(m_es_format);
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
}


COpenMaxVideoBuffer* COpenMaxVideoBuffer::Acquire()
{
  long count = AtomicIncrement(&m_refs);
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __func__, this, mmal_buffer, count);
  #endif
  (void)count;
  return this;
}

long COpenMaxVideoBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
#if defined(OMX_DEBUG_VERBOSE)
CLog::Log(LOGDEBUG, "%s::%s %p (%p) ref:%ld", CLASSNAME, __func__, this, mmal_buffer, count);
#endif
  if (count == 0)
  {
    m_omv->ReleaseOpenMaxBuffer(this);
  }
  return count;
}

#undef CLASSNAME
#define CLASSNAME "COpenMaxVideo"

COpenMaxVideo::COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  pthread_mutex_init(&m_omx_output_mutex, NULL);

  m_drop_state = false;
  m_decoded_width = 0;
  m_decoded_height = 0;

  m_finished = false;
  m_pFormatName = "omx-xxxx";

  m_deinterlace_enabled = false;
  m_deinterlace_request = VS_DEINTERLACEMODE_OFF;
  m_decoderPts = DVD_NOPTS_VALUE;
  m_droppedPics = 0;
  m_decode_frame_number = 1;
  m_skipDeinterlaceFields = false;


  m_dec = NULL;
  m_dec_input = NULL;
  m_dec_output = NULL;
  m_dec_input_pool = NULL;
  m_dec_output_pool = NULL;

  m_es_format = NULL;

  m_codingType = 0;

  m_changed_count = 0;
  m_changed_count_dec = 0;
  m_omx_output_busy = 0;
}

COpenMaxVideo::~COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  assert(m_finished);

#ifdef DTS_QUEUE
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
#endif

  pthread_mutex_destroy(&m_omx_output_mutex);

  if (m_dec)
  {
    mmal_component_disable(m_dec);
    mmal_port_disable(m_dec->control);
  }

  if (m_dec_input)
    mmal_port_disable(m_dec_input);

  if (m_dec_output)
    mmal_port_disable(m_dec_output);

  if (m_dec_input_pool)
    mmal_pool_destroy(m_dec_input_pool);

  if (m_dec_output_pool)
    mmal_pool_destroy(m_dec_output_pool);

  if (m_dec)
    mmal_component_release(m_dec);

  if (m_es_format)
    mmal_format_free(m_es_format);
}

static void dec_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  MMAL_STATUS_T status;

  if (buffer->cmd == MMAL_EVENT_ERROR)
  {
    status = (MMAL_STATUS_T)*(uint32_t *)buffer->data;
    CLog::Log(LOGERROR, "%s::%s Error (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
  }

  mmal_buffer_header_release(buffer);
}

static void dec_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);
#endif
  mmal_buffer_header_release(buffer);
}


void COpenMaxVideo::dec_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  if (!(buffer->cmd == 0 && buffer->length > 0))
    CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);
#endif
  bool kept = false;

  if (buffer->cmd == 0)
  {
    if (buffer->length > 0)
    {
      assert(!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));
      double dts = DVD_NOPTS_VALUE;
      #ifdef DTS_QUEUE
      pthread_mutex_lock(&m_omx_output_mutex);
      if (!m_dts_queue.empty())
      {
        dts = m_dts_queue.front();
        m_dts_queue.pop();
      }
      else assert(0);
      pthread_mutex_unlock(&m_omx_output_mutex);
      #endif

static int count;
      //if (++count > 5 || m_drop_state)
      if (m_drop_state)
      {
        CLog::Log(LOGDEBUG, "%s::%s - dropping %p (drop:%d)", CLASSNAME, __func__, buffer, m_drop_state);
      }
      else
      {
        COpenMaxVideoBuffer *omvb = new COpenMaxVideoBuffer(this);
        m_omx_output_busy++;
#if defined(OMX_DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s::%s - %p (%p) buffer_size(%u) dts:%.3f pts:%.3f flags:%x:%x frame:%d",
        CLASSNAME, __func__, buffer, omvb, buffer->length, dts*1e-6, buffer->pts*1e-6, buffer->flags, buffer->type->video.flags, omvb->m_changed_count);
#endif
        omvb->mmal_buffer = buffer;
        buffer->user_data = (void *)omvb;
        mmal_format_full_copy(omvb->m_es_format, m_es_format);
        omvb->m_changed_count = m_changed_count;
        omvb->dts = dts;
        omvb->width = m_decoded_width;
        omvb->height = m_decoded_height;
        omvb->m_aspect_ratio = m_aspect_ratio;
        pthread_mutex_lock(&m_omx_output_mutex);
        m_omx_output_ready.push(omvb);
        pthread_mutex_unlock(&m_omx_output_mutex);
        kept = true;
      }
    }
  }
  else if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
  {
    MMAL_EVENT_FORMAT_CHANGED_T *fmt = mmal_event_format_changed_get(buffer);
    mmal_format_full_copy(m_es_format, fmt->format);
    m_es_format->encoding = MMAL_ENCODING_OPAQUE;
    m_changed_count++;

    if (m_es_format->es->video.par.num && m_es_format->es->video.par.den)
      m_aspect_ratio = (float)(m_es_format->es->video.par.num * m_es_format->es->video.width) / (m_es_format->es->video.par.den * m_es_format->es->video.height);
    m_decoded_width = m_es_format->es->video.width;
    m_decoded_height = m_es_format->es->video.height;
    CLog::Log(LOGDEBUG, "%s::%s format changed: %dx%d %.2f frame:%d", CLASSNAME, __func__, m_decoded_width, m_decoded_height, m_aspect_ratio, m_changed_count);
  }
  if (!kept)
    mmal_buffer_header_release(buffer);
}

static void dec_output_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  COpenMaxVideo *omx = reinterpret_cast<COpenMaxVideo*>(port->userdata);
  omx->dec_output_port_cb(port, buffer);
}

static void* pool_allocator_alloc(void *context, uint32_t size)
{
  return mmal_port_payload_alloc((MMAL_PORT_T *)context, size);
}

static void pool_allocator_free(void *context, void *mem)
{
  mmal_port_payload_free((MMAL_PORT_T *)context, (uint8_t *)mem);
}

int COpenMaxVideo::change_dec_output_format()
{
  MMAL_STATUS_T status;
  int ret = 0;
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  status = mmal_port_disable(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  mmal_format_full_copy(m_dec_output->format, m_es_format);
  status = mmal_port_format_commit(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit decoder output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

  //m_dec_output->buffer_num = 40;
  //m_dec_output->buffer_size = m_dec_output->buffer_size_min;
  status = mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    ret = -1;
    goto out;
  }

out:
    return ret;
}

bool COpenMaxVideo::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, OpenMaxVideoPtr myself)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s useomx:%d software:%d %dx%d", CLASSNAME, __func__, CSettings::Get().GetBool("videoplayer.useomx"), hints.software, hints.width, hints.height);
  #endif

  // we always qualify even if DVDFactoryCodec does this too.
  if (!CSettings::Get().GetBool("videoplayer.useomx") || hints.software)
    return false;

  m_hints = hints;
  MMAL_STATUS_T status;
  MMAL_PARAMETER_BOOLEAN_T error_concealment;

  m_deinterlace_request = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;

  m_myself = myself;
  m_decoded_width  = hints.width;
  m_decoded_height = hints.height;
  m_forced_aspect_ratio = hints.forced_aspect;
  m_aspect_ratio = hints.aspect;

  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      // H.264
      m_codingType = MMAL_ENCODING_H264;
      m_pFormatName = "omx-h264";
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_codingType = MMAL_ENCODING_MP4V;
      m_pFormatName = "omx-mpeg4";
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_codingType = MMAL_ENCODING_MP2V;
      m_pFormatName = "omx-mpeg2";
    break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // VP6
      m_codingType = MMAL_ENCODING_VP6;
      m_pFormatName = "omx-vp6";
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_codingType = MMAL_ENCODING_VP8;
      m_pFormatName = "omx-vp8";
    break;
    case AV_CODEC_ID_THEORA:
      // theora
      m_codingType = MMAL_ENCODING_THEORA;
      m_pFormatName = "omx-theora";
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // mjpg
      m_codingType = MMAL_ENCODING_MJPEG;
      m_pFormatName = "omx-mjpg";
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // VC-1, WMV9
      m_codingType = MMAL_ENCODING_WVC1;
      m_pFormatName = "omx-vc1";
      break;
    default:
      CLog::Log(LOGERROR, "%s::%s : Video codec unknown: %x", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  if ( (m_codingType == MMAL_ENCODING_MP2V && !g_RBP.GetCodecMpg2() ) ||
       (m_codingType == MMAL_ENCODING_WVC1   && !g_RBP.GetCodecWvc1() ) )
  {
    CLog::Log(LOGWARNING, "%s::%s Codec %s is not supported", CLASSNAME, __func__, m_pFormatName);
    return false;
  }

  // initialize mmal.
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create MMAL decoder component %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec->control->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec->control, dec_control_port_cb);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder control port %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_input = m_dec->input[0];

  m_dec_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  m_dec_input->format->type = MMAL_ES_TYPE_VIDEO;
  m_dec_input->format->encoding = m_codingType;
  if (m_hints.width && m_hints.height)
  {
    m_dec_input->format->es->video.width = m_hints.width;
    m_dec_input->format->es->video.height = m_hints.height;
  }
  m_dec_input->format->flags |= MMAL_ES_FORMAT_FLAG_FRAMED;

  error_concealment.hdr.id = MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT;
  error_concealment.hdr.size = sizeof(MMAL_PARAMETER_BOOLEAN_T);
  error_concealment.enable = MMAL_FALSE;
  status = mmal_port_parameter_set(m_dec_input, &error_concealment.hdr);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to disable error concealment on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_port_parameter_set_uint32(m_dec_input, MMAL_PARAMETER_EXTRA_BUFFERS, GetAllowedReferences() + 4);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable extra buffers on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_format_extradata_alloc(m_dec_input->format, hints.extrasize);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s failed to allocate extradata", CLASSNAME, __func__);
    return false;
  }
  m_dec_input->format->extradata_size = hints.extrasize;
  if (m_dec_input->format->extradata_size)
     memcpy(m_dec_input->format->extradata, hints.extradata, m_dec_input->format->extradata_size);

  status = mmal_port_format_commit(m_dec_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit format for decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }
  m_dec_input->buffer_size = m_dec_input->buffer_size_recommended;
  m_dec_input->buffer_num = m_dec_input->buffer_num_recommended;

  status = mmal_port_enable(m_dec_input, dec_input_port_cb);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output = m_dec->output[0];
  m_dec_output->userdata = (struct MMAL_PORT_USERDATA_T *)this;


  // set up initial decoded frame format - will likely change from this
  m_es_format = mmal_format_alloc();
  mmal_format_full_copy(m_es_format, m_dec_output->format);

  m_es_format->encoding = MMAL_ENCODING_OPAQUE;
  m_es_format->type = MMAL_ES_TYPE_VIDEO;
  if (m_hints.width && m_hints.height)
  {
    m_es_format->es->video.width = m_hints.width;
    m_es_format->es->video.height = m_hints.height;
    m_es_format->es->video.crop.width = m_hints.width;
    m_es_format->es->video.crop.height = m_hints.height;
  }

  mmal_format_full_copy(m_dec_output->format, m_es_format);
  status = mmal_port_format_commit(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit decoder output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output->buffer_size = m_dec_output->buffer_size_min;
  m_dec_output->buffer_num= m_dec_output->buffer_num_recommended;
  status = mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_enable(m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder component %s (status=%x %s)", CLASSNAME, __func__, m_dec->name, status, mmal_status_to_string(status));
    return false;
  }

  printf("m_dec_input_pool: %dx%d=%dM\n", m_dec_input->buffer_num, m_dec_input->buffer_size, m_dec_input->buffer_num * m_dec_input->buffer_size >> 20);
  m_dec_input_pool = mmal_pool_create_with_allocator(m_dec_input->buffer_num, m_dec_input->buffer_size, m_dec_input, pool_allocator_alloc, pool_allocator_free);
  if (!m_dec_input_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  printf("m_dec_output_pool: %dx%d=%dM\n", m_dec_output->buffer_num, m_dec_output->buffer_size, m_dec_output->buffer_num * m_dec_output->buffer_size >> 20);
  m_dec_output_pool = mmal_pool_create_with_allocator(m_dec_output->buffer_num, m_dec_output->buffer_size, m_dec_output, pool_allocator_alloc, pool_allocator_free);
  if(!m_dec_output_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for decode output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  m_drop_state = false;

  return true;
}

void COpenMaxVideo::Dispose()
{
  // we are happy to exit, but let last shared pointer being deleted trigger the destructor
  bool done = false;
  Reset();
  pthread_mutex_lock(&m_omx_output_mutex);
  if (!m_omx_output_busy)
    done = true;
  m_finished = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s dts_queue(%d) ready_queue(%d) busy_queue(%d) done:%d", CLASSNAME, __func__, m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy, done);
  #endif
  if (done)
  {
    assert(m_dts_queue.empty());
    m_myself.reset();
  }
}

void COpenMaxVideo::SetDropState(bool bDrop)
{
#if defined(OMX_DEBUG_VERBOSE)
  if (m_drop_state != bDrop)
    CLog::Log(LOGDEBUG, "%s::%s - m_drop_state(%d)", CLASSNAME, __func__, bDrop);
#endif
  m_drop_state = bDrop;
  if (m_drop_state)
  {
    while (1)
    {
      COpenMaxVideoBuffer *buffer = NULL;
      pthread_mutex_lock(&m_omx_output_mutex);
      // fetch a output buffer and pop it off the ready list
      if (!m_omx_output_ready.empty())
      {
        buffer = m_omx_output_ready.front();
        m_omx_output_ready.pop();
      }
      pthread_mutex_unlock(&m_omx_output_mutex);
      if (buffer)
        ReleaseOpenMaxBuffer(buffer);
      else
        break;
    }
  }
}

int COpenMaxVideo::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  #if defined(OMX_DEBUG_VERBOSE)
  //CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f dts_queue(%d) ready_queue(%d) busy_queue(%d)",
  //   CLASSNAME, __func__, pData, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy);
  #endif

  unsigned int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  MMAL_BUFFER_HEADER_T *buffer;
  MMAL_STATUS_T status;

  // we need to queue then de-queue the demux packet, seems silly but
  // omx might not have a omx input buffer available when we are called
  // and we must store the demuxer packet and try again later.
  while (demuxer_bytes)
  {
      // 500ms timeout
      buffer = mmal_queue_timedwait(m_dec_input_pool->queue, 500);
      if (!buffer)
      {
        CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __func__);
        return VC_ERROR;
      }

      mmal_buffer_header_reset(buffer);
      buffer->cmd = 0;
      buffer->pts = pts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : pts;
      buffer->dts = dts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : dts;
      buffer->length = demuxer_bytes > buffer->alloc_size ? buffer->alloc_size : demuxer_bytes;
      buffer->user_data = (void *)m_decode_frame_number;

      if (0 && m_drop_state)
      {
        // Request decode only (maintain ref frames, but don't return a picture)
        buffer->flags |= MMAL_BUFFER_HEADER_FLAG_DECODEONLY;
        m_droppedPics += m_deinterlace_enabled ? 2:1;
      }
      memcpy(buffer->data, demuxer_content, buffer->length);
      demuxer_bytes   -= buffer->length;
      demuxer_content += buffer->length;

      if (demuxer_bytes == 0)
        buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

      #if defined(OMX_DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f flags:%x dts_queue(%d) ready_queue(%d) busy_queue(%d)",
         CLASSNAME, __func__, buffer, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, buffer->flags, m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy);
      #endif
      status = mmal_port_send_buffer(m_dec_input, buffer);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed send buffer to decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return VC_ERROR;
      }

      if (demuxer_bytes == 0)
      {
        m_decode_frame_number++;
#ifdef DTS_QUEUE
        if (!(0 && m_drop_state))
        {
          // only push if we are successful with feeding OMX_EmptyThisBuffer
          pthread_mutex_lock(&m_omx_output_mutex);
          m_dts_queue.push(dts);
          //assert(m_dts_queue.size() < 100);
          pthread_mutex_unlock(&m_omx_output_mutex);
        }
#endif
        if (m_changed_count_dec != m_changed_count)
        {
          CLog::Log(LOGDEBUG, "%s::%s format changed frame:%d(%d)", CLASSNAME, __func__, m_changed_count_dec, m_changed_count);
          m_changed_count_dec = m_changed_count;
          if (change_dec_output_format() < 0)
          {
            CLog::Log(LOGERROR, "%s::%s - change_dec_output_format() failed", CLASSNAME, __func__);
            return VC_ERROR;
          }
        }
        while (buffer = mmal_queue_get(m_dec_output_pool->queue), buffer)
          Recycle(buffer);
      }
  }
  if (m_omx_output_ready.empty())
  {
    //CLog::Log(LOGDEBUG, "%s::%s - empty: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
    return VC_BUFFER;
  }

  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
  return VC_PICTURE;
}

void COpenMaxVideo::Reset(void)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  mmal_port_flush(m_dec_input);
  mmal_port_flush(m_dec_output);

  // blow all ready video frames
  SetDropState(true);
  SetDropState(false);
#ifdef DTS_QUEUE
  pthread_mutex_lock(&m_omx_output_mutex);
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
  pthread_mutex_unlock(&m_omx_output_mutex);
#endif

  m_decoderPts = DVD_NOPTS_VALUE;
  m_droppedPics = 0;
  m_decode_frame_number = 1;
}


void COpenMaxVideo::ReturnOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%d)", CLASSNAME, __func__, buffer, m_omx_output_busy);
#endif

  mmal_buffer_header_release(buffer->mmal_buffer);
}

void COpenMaxVideo::Recycle(MMAL_BUFFER_HEADER_T *buffer)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, buffer);
#endif

  MMAL_STATUS_T status;
  mmal_buffer_header_reset(buffer);
  buffer->cmd = 0;
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s Send buffer %p from pool to decoder output port %p dts_queue(%d) ready_queue(%d) busy_queue(%d)", CLASSNAME, __func__, buffer, m_dec_output,
    m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy);
  #endif
  status = mmal_port_send_buffer(m_dec_output, buffer);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed send buffer to decoder output port (status=0%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return;
  }
}

void COpenMaxVideo::ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
  // remove from busy list
  pthread_mutex_lock(&m_omx_output_mutex);
  assert(m_omx_output_busy > 0);
  m_omx_output_busy--;
  pthread_mutex_unlock(&m_omx_output_mutex);
  ReturnOpenMaxBuffer(buffer);
  bool done = false;
  pthread_mutex_lock(&m_omx_output_mutex);
  if (m_finished && !m_omx_output_busy)
    done = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  if (done)
    m_myself.reset();
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%p) dts_queue(%d) ready_queue(%d) busy_queue(%d) done:%d", CLASSNAME, __func__, buffer, buffer->mmal_buffer, m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy, done);
  #endif
  delete buffer;
}

bool COpenMaxVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  //CLog::Log(LOGDEBUG, "%s::%s - m_omx_output_busy=%d m_omx_output_ready.size()=%d", CLASSNAME, __func__, m_omx_output_busy, m_omx_output_ready.size());
  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());

  if (!m_omx_output_ready.empty())
  {
    COpenMaxVideoBuffer *buffer;
    // fetch a output buffer and pop it off the ready list
    pthread_mutex_lock(&m_omx_output_mutex);
    buffer = m_omx_output_ready.front();
    m_omx_output_ready.pop();
    pthread_mutex_unlock(&m_omx_output_mutex);

    assert(buffer->mmal_buffer);
    memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
    pDvdVideoPicture->format = RENDER_FMT_OMXEGL;
    pDvdVideoPicture->openMaxBuffer = buffer;
    pDvdVideoPicture->color_range  = 0;
    pDvdVideoPicture->color_matrix = 4;
    pDvdVideoPicture->iWidth  = buffer->width ? buffer->width : m_decoded_width;
    pDvdVideoPicture->iHeight = buffer->height ? buffer->height : m_decoded_height;
    pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
    pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;
    //CLog::Log(LOGDEBUG, "%s::%s -  %dx%d %dx%d %dx%d %dx%d %d,%d %f,%f", CLASSNAME, __func__, pDvdVideoPicture->iWidth, pDvdVideoPicture->iHeight, pDvdVideoPicture->iDisplayWidth, pDvdVideoPicture->iDisplayHeight, m_decoded_width, m_decoded_height, buffer->width, buffer->height, m_forced_aspect_ratio, m_hints.forced_aspect, buffer->m_aspect_ratio, m_hints.aspect);

    if (buffer->m_aspect_ratio > 0.0 && !m_forced_aspect_ratio)
    {
      pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * buffer->m_aspect_ratio)) & -3;
      if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
      {
        pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
        pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / buffer->m_aspect_ratio)) & -3;
      }
    }

    // timestamp is in microseconds
    pDvdVideoPicture->dts = buffer->dts;
    pDvdVideoPicture->pts = buffer->mmal_buffer->pts == MMAL_TIME_UNKNOWN ? DVD_NOPTS_VALUE : buffer->mmal_buffer->pts;

    pDvdVideoPicture->openMaxBuffer->Acquire();
    pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
#if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGINFO, "%s::%s dts:%.3f pts:%.3f flags:%x:%x openMaxBuffer:%p mmal_buffer:%p", CLASSNAME, __func__,
        pDvdVideoPicture->dts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->dts*1e-6, pDvdVideoPicture->pts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->pts*1e-6,
        pDvdVideoPicture->iFlags, buffer->mmal_buffer->flags, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer->mmal_buffer);
#endif
    assert(!(buffer->mmal_buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));
  }
  else
  {
    CLog::Log(LOGERROR, "%s::%s - called but m_omx_output_ready is empty", CLASSNAME, __func__);
    return false;
  }

  if (pDvdVideoPicture->pts != DVD_NOPTS_VALUE)
    m_decoderPts = pDvdVideoPicture->pts;
  else
    m_decoderPts = pDvdVideoPicture->dts; // xxx is DVD_NOPTS_VALUE better?

  return true;
}

bool COpenMaxVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - %p (%p)", CLASSNAME, __func__, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer ? pDvdVideoPicture->openMaxBuffer->mmal_buffer : 0);
#endif
  if (pDvdVideoPicture->format == RENDER_FMT_OMXEGL)
  {
    pDvdVideoPicture->openMaxBuffer->Release();
  }
  memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
  return true;
}

bool COpenMaxVideo::GetCodecStats(double &pts, int &droppedPics)
{
  pts = m_decoderPts;
  droppedPics = m_droppedPics;
  m_droppedPics = 0;
#if defined(OMX_DEBUG_VERBOSE)
  //CLog::Log(LOGDEBUG, "%s::%s - pts:%.0f droppedPics:%d", CLASSNAME, __func__, pts, droppedPics);
#endif
  return true;
}

