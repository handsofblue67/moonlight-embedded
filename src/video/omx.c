/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  * Neither the name of the copyright holder nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Video decode on Raspberry Pi using OpenMAX IL though the ilcient helper library
// Based upon video decode example from the Raspberry Pi firmware

#include "../video.h"

#include "bcm_host.h"
#include "ilclient.h"
#include "h264_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TUNNEL_T tunnel[2];
static COMPONENT_T *list[3];
static ILCLIENT_T *client;

static COMPONENT_T *video_decode = NULL, *video_scheduler = NULL, *video_render = NULL;

static OMX_BUFFERHEADERTYPE *buf;
static unsigned char *dest;

static int port_settings_changed;
static int first_packet;

static h264_stream_t* stream;

static void decoder_renderer_setup(int width, int height, int redrawRate, void* context, int drFlags) {
  stream = h264_new();
  if (stream == NULL) {
    fprintf(stderr, "Not enough memory\n");
    exit(EXIT_FAILURE);
  }

  bcm_host_init();

  OMX_VIDEO_PARAM_PORTFORMATTYPE format;
  OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
  COMPONENT_T *clock = NULL;
  unsigned int data_len = 0;
  int packet_size = 80<<10;

  memset(list, 0, sizeof(list));
  memset(tunnel, 0, sizeof(tunnel));

  if((client = ilclient_init()) == NULL) {
    fprintf(stderr, "Can't initialize video\n");
    exit(EXIT_FAILURE);
  }

  if(OMX_Init() != OMX_ErrorNone) {
    fprintf(stderr, "Can't initialize OMX\n");
    exit(EXIT_FAILURE);
  }

  // create video_decode
  if(ilclient_create_component(client, &video_decode, "video_decode", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0){
    fprintf(stderr, "Can't create video decode\n");
    exit(EXIT_FAILURE);
  }

  list[0] = video_decode;

  // create video_render
  if(ilclient_create_component(client, &video_render, "video_render", ILCLIENT_DISABLE_ALL_PORTS) != 0){
    fprintf(stderr, "Can't create video renderer\n");
    exit(EXIT_FAILURE);
  }

  list[1] = video_render;

  set_tunnel(tunnel, video_decode, 131, video_render, 90);

  ilclient_change_component_state(video_decode, OMX_StateIdle);

  memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
  format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
  format.nVersion.nVersion = OMX_VERSION;
  format.nPortIndex = 130;
  format.eCompressionFormat = OMX_VIDEO_CodingAVC;

  OMX_PARAM_DATAUNITTYPE unit;

  memset(&unit, 0, sizeof(OMX_PARAM_DATAUNITTYPE));
  unit.nSize = sizeof(OMX_PARAM_DATAUNITTYPE);
  unit.nVersion.nVersion = OMX_VERSION;
  unit.nPortIndex = 130;
  unit.eUnitType = OMX_DataUnitCodedPicture;
  unit.eEncapsulationType = OMX_DataEncapsulationElementaryStream;

  if(OMX_SetParameter(ILC_GET_HANDLE(video_decode), OMX_IndexParamVideoPortFormat, &format) == OMX_ErrorNone &&
     OMX_SetParameter(ILC_GET_HANDLE(video_decode), OMX_IndexParamBrcmDataUnit, &unit) == OMX_ErrorNone &&
     ilclient_enable_port_buffers(video_decode, 130, NULL, NULL, NULL) == 0) {

    port_settings_changed = 0;
    first_packet = 1;

    ilclient_change_component_state(video_decode, OMX_StateExecuting);
  } else {
    fprintf(stderr, "Can't setup video\n");
    exit(EXIT_FAILURE);
  }
}

static void decoder_renderer_stop() {
  int status = 0;

  buf->nFilledLen = 0;
  buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;

  if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(list[0]), buf) != OMX_ErrorNone){
    fprintf(stderr, "Can't empty video buffer\n");
    return;
  }

  // need to flush the renderer to allow video_decode to disable its input port
  ilclient_flush_tunnels(tunnel, 0);

  ilclient_disable_port_buffers(list[0], 130, NULL, NULL, NULL);
}

static void decoder_renderer_release() {
  ilclient_disable_tunnel(tunnel);
  ilclient_teardown_tunnels(tunnel);

  ilclient_state_transition(list, OMX_StateIdle);
  ilclient_state_transition(list, OMX_StateLoaded);

  ilclient_cleanup_components(list);

  OMX_Deinit();

  ilclient_destroy(client);
}

static int decoder_renderer_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if((buf = ilclient_get_input_buffer(video_decode, 130, 1)) == NULL){
    fprintf(stderr, "Can't get video buffer\n");
    exit(EXIT_FAILURE);
  }

  // feed data and wait until we get port settings changed
  dest = buf->pBuffer;

  buf->nFilledLen = 0;

  buf->nOffset = 0;

  buf->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS;

  if(first_packet) {
    buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
    first_packet = 0;
  }

  PLENTRY entry = decodeUnit->bufferList;
  if (entry != NULL && entry->data[4] == 0x67) {
    read_nal_unit(stream, entry->data+4, entry->length-4);
    stream->sps->num_ref_frames = 1;
    stream->sps->vui.bitstream_restriction_flag = 1;
    stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
    stream->sps->vui.max_bytes_per_pic_denom = 2;
    stream->sps->vui.max_bits_per_mb_denom = 1;
    stream->sps->vui.log2_max_mv_length_horizontal = 16;
    stream->sps->vui.log2_max_mv_length_vertical = 16;
    stream->sps->vui.num_reorder_frames = 0;
    stream->sps->vui.max_dec_frame_buffering = 1;

    entry->length = write_nal_unit(stream, entry->data+4, entry->length*2) + 4;
  }

  while (entry != NULL) {
    memcpy(dest, entry->data, entry->length);
    buf->nFilledLen += entry->length;
    dest += entry->length;
    entry = entry->next;
  }

  if(port_settings_changed == 0 &&
    ((buf->nFilledLen > 0 && ilclient_remove_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) ||
    (buf->nFilledLen == 0 && ilclient_wait_for_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1,
                        ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0))) {
    port_settings_changed = 1;

    if(ilclient_setup_tunnel(tunnel, 0, 0) != 0){
      fprintf(stderr, "Can't setup video\n");
      exit(EXIT_FAILURE);
    }

    ilclient_change_component_state(video_render, OMX_StateExecuting);
  }

  if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone){
    fprintf(stderr, "Can't empty video buffer\n");
    exit(EXIT_FAILURE);
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_omx = {
  .setup = decoder_renderer_setup,
  .start = NULL,
  .stop = decoder_renderer_stop,
  .release = decoder_renderer_release,
  .submitDecodeUnit = decoder_renderer_submit_decode_unit,
};
