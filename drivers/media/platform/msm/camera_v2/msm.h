/* Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MSM_H
#define _MSM_H

#include <linux/version.h>
#include <linux/completion.h>
#include <linux/i2c.h>
#include <linux/videodev2.h>
#include <linux/pm_qos.h>
#include <linux/msm_ion.h>
#include <linux/iommu.h>
#include <linux/msm_kgsl.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mediabus.h>
#include <media/videobuf2-dma-contig.h>
#include <media/msmb_camera.h>

#ifdef CONFIG_MACH_LGE
/* LGE_CHANGE_S, camera stability task, added  msm-config debugfs*/
#include <linux/debugfs.h>
#define LGE_DEBUG_DISABLE_TIMEOUT	1
#define LGE_DEBUG_PANIC_ON_TIMEOUT	2
#define LGE_DEBUG_BLOCK_POST_EVENT	7
#define BIT_SET( x, idx )    ( x |= 1<<(idx&7))
#define BIT_ISSET( x, idx )  ( x & (1<<(idx&7)))
#define BIT_CLR( x, idx )    ( x &= ~(1<<(idx&7)))
/* LGE_CHANGE_E, camera stability task, added  msm-config debugfs*/
#endif

/* Setting MAX timeout to 6.5seconds considering
 * backend will operate @ .6fps in certain usecases
 * like Long exposure usecase and isp needs max of 2 frames
 * to stop the hardware which will be around 3 seconds*/
#define MSM_POST_EVT_TIMEOUT 6500
#define MSM_POST_EVT_NOTIMEOUT 0xFFFFFFFF
#define MSM_CAMERA_STREAM_CNT_BITS  32

#define CAMERA_ENABLE_PC_LATENCY PM_QOS_DEFAULT_VALUE

extern bool is_daemon_status;

struct msm_video_device {
	struct video_device *vdev;
	atomic_t opened;
	struct mutex video_drvdata_mutex;
};

struct msm_queue_head {
	struct list_head list;
	spinlock_t lock;
	int len;
	int max;
};

/** msm_event:
 *
 *  event sent by imaging server
 **/
struct msm_event {
	struct video_device *vdev;
	atomic_t on_heap;
};

struct msm_command {
	struct list_head list;
	struct v4l2_event event;
	atomic_t on_heap;
};

/** struct msm_command_ack
 *
 *  Object of command_ack_q, which is
 *  created per open operation
 *
 *  contains struct msm_command
 **/
struct msm_command_ack {
	struct list_head list;
	struct msm_queue_head command_q;
	struct completion wait_complete;
	int stream_id;
};

struct msm_v4l2_subdev {
	/* FIXME: for session close and error handling such
	 * as daemon shutdown */
	int    close_sequence;
};

struct msm_session {
	struct list_head list;

	/* session index */
	unsigned int session_id;

	/* event queue sent by imaging server */
	struct msm_event event_q;

	/* ACK by imaging server. Object type of
	 * struct msm_command_ack per open,
	 * assumption is application can send
	 * command on every opened video node */
	struct msm_queue_head command_ack_q;

	/* real streams(either data or metadate) owned by one
	 * session struct msm_stream */
	struct msm_queue_head stream_q;
	struct mutex lock;
	struct mutex lock_q;
	struct mutex close_lock;
	rwlock_t stream_rwlock;
	struct kgsl_pwr_limit *sysfs_pwr_limit;
};

static inline bool msm_is_daemon_present(void)
{
	return is_daemon_status;
}

int msm_post_event(struct v4l2_event *event, int timeout);
int  msm_create_session(unsigned int session, struct video_device *vdev);
int msm_destroy_session(unsigned int session_id);

int msm_create_stream(unsigned int session_id,
	unsigned int stream_id, struct vb2_queue *q);
void msm_delete_stream(unsigned int session_id, unsigned int stream_id);
int  msm_create_command_ack_q(unsigned int session_id, unsigned int stream_id);
void msm_delete_command_ack_q(unsigned int session_id, unsigned int stream_id);
struct msm_session *msm_get_session(unsigned int session_id);
struct msm_stream *msm_get_stream(struct msm_session *session,
	unsigned int stream_id);
struct vb2_queue *msm_get_stream_vb2q(unsigned int session_id,
	unsigned int stream_id);
struct msm_stream *msm_get_stream_from_vb2q(struct vb2_queue *q);
struct msm_session *msm_get_session_from_vb2q(struct vb2_queue *q);
struct msm_session *msm_session_find(unsigned int session_id);
#ifdef CONFIG_COMPAT
long msm_copy_camera_private_ioctl_args(unsigned long arg,
	struct msm_camera_private_ioctl_arg *k_ioctl,
	void __user **tmp_compat_ioctl_ptr);
#endif
#endif /*_MSM_H */
