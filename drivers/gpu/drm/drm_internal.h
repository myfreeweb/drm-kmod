/*
 * Copyright © 2014 Intel Corporation
 *   Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_ioctl.h>

#ifdef __FreeBSD__
#include <drm/drm_file.h>	/* Needed by the debugfs funcs */
#endif

#define DRM_IF_MAJOR 1
#define DRM_IF_MINOR 4

#define DRM_IF_VERSION(maj, min) (maj << 16 | min)

struct dentry;
struct dma_buf;
struct drm_connector;
struct drm_crtc;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_master;
struct drm_minor;
struct drm_prime_file_private;
struct drm_printer;

/* drm_file.c */
extern struct mutex drm_global_mutex;
struct drm_file *drm_file_alloc(struct drm_minor *minor);
void drm_file_free(struct drm_file *file);
void drm_lastclose(struct drm_device *dev);

#ifdef CONFIG_PCI

/* drm_pci.c */
int drm_irq_by_busid(struct drm_device *dev, void *data,
		     struct drm_file *file_priv);
#ifdef __FreeBSD__
int drm_getpciinfo(struct drm_device *dev, void *data,
		     struct drm_file *file_priv);
#endif
void drm_pci_agp_destroy(struct drm_device *dev);
int drm_pci_set_busid(struct drm_device *dev, struct drm_master *master);

#else

static inline int drm_irq_by_busid(struct drm_device *dev, void *data,
				   struct drm_file *file_priv)
{
	return -EINVAL;
}

static inline void drm_pci_agp_destroy(struct drm_device *dev)
{
}

static inline int drm_pci_set_busid(struct drm_device *dev,
				    struct drm_master *master)
{
	return -EINVAL;
}

#endif

/* drm_prime.c */
int drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);
int drm_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);

void drm_prime_init_file_private(struct drm_prime_file_private *prime_fpriv);
void drm_prime_destroy_file_private(struct drm_prime_file_private *prime_fpriv);
void drm_prime_remove_buf_handle_locked(struct drm_prime_file_private *prime_fpriv,
					struct dma_buf *dma_buf);

/* drm_drv.c */
struct drm_minor *drm_minor_acquire(unsigned int minor_id);
void drm_minor_release(struct drm_minor *minor);

/* drm_vblank.c */
void drm_vblank_disable_and_save(struct drm_device *dev, unsigned int pipe);
void drm_vblank_cleanup(struct drm_device *dev);

/* IOCTLS */
int drm_wait_vblank_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp);
int drm_legacy_modeset_ctl_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);

/* drm_irq.c */

/* IOCTLS */
#if IS_ENABLED(CONFIG_DRM_LEGACY)
int drm_legacy_irq_control(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
#endif

int drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp);

int drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *filp);

/* drm_auth.c */
int drm_getmagic(struct drm_device *dev, void *data,
		 struct drm_file *file_priv);
int drm_authmagic(struct drm_device *dev, void *data,
		  struct drm_file *file_priv);
int drm_setmaster_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int drm_dropmaster_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv);
int drm_master_open(struct drm_file *file_priv);
void drm_master_release(struct drm_file *file_priv);
bool drm_master_internal_acquire(struct drm_device *dev);
void drm_master_internal_release(struct drm_device *dev);

/* drm_sysfs.c */
extern struct class *drm_class;

int drm_sysfs_init(void);
void drm_sysfs_destroy(void);
struct device *drm_sysfs_minor_alloc(struct drm_minor *minor);
int drm_sysfs_connector_add(struct drm_connector *connector);
void drm_sysfs_connector_remove(struct drm_connector *connector);

void drm_sysfs_lease_event(struct drm_device *dev);

/* drm_gem.c */
struct drm_gem_object;
int drm_gem_init(struct drm_device *dev);
void drm_gem_destroy(struct drm_device *dev);
int drm_gem_handle_create_tail(struct drm_file *file_priv,
			       struct drm_gem_object *obj,
			       u32 *handlep);
int drm_gem_close_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int drm_gem_flink_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int drm_gem_open_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv);
void drm_gem_open(struct drm_device *dev, struct drm_file *file_private);
void drm_gem_release(struct drm_device *dev, struct drm_file *file_private);
void drm_gem_print_info(struct drm_printer *p, unsigned int indent,
			const struct drm_gem_object *obj);

int drm_gem_pin(struct drm_gem_object *obj);
void drm_gem_unpin(struct drm_gem_object *obj);
void *drm_gem_vmap(struct drm_gem_object *obj);
void drm_gem_vunmap(struct drm_gem_object *obj, void *vaddr);

/* drm_debugfs.c drm_debugfs_crc.c */
#if defined(CONFIG_DEBUG_FS)
int drm_debugfs_init(struct drm_minor *minor, int minor_id,
		     struct dentry *root);
void drm_debugfs_cleanup(struct drm_minor *minor);
void drm_debugfs_connector_add(struct drm_connector *connector);
void drm_debugfs_connector_remove(struct drm_connector *connector);
void drm_debugfs_crtc_add(struct drm_crtc *crtc);
void drm_debugfs_crtc_remove(struct drm_crtc *crtc);
void drm_debugfs_crtc_crc_add(struct drm_crtc *crtc);
#else
/* Need to find a proper way to do that */
int drm_sysctl_init(struct drm_device *dev);
int drm_sysctl_cleanup(struct drm_device *dev);
static inline int drm_debugfs_init(struct drm_minor *minor, int minor_id,
				   struct dentry *root)
{
#ifdef __FreeBSD__
	if (minor->type != DRM_MINOR_RENDER)
		drm_sysctl_init(minor->dev);
#endif
	return 0;
}

static inline void drm_debugfs_cleanup(struct drm_minor *minor)
{
#ifdef __FreeBSD__
	if (minor->type != DRM_MINOR_RENDER)
		drm_sysctl_cleanup(minor->dev);
#endif
}

static inline void drm_debugfs_connector_add(struct drm_connector *connector)
{
}
static inline void drm_debugfs_connector_remove(struct drm_connector *connector)
{
}

static inline void drm_debugfs_crtc_add(struct drm_crtc *crtc)
{
}
static inline void drm_debugfs_crtc_remove(struct drm_crtc *crtc)
{
}

static inline void drm_debugfs_crtc_crc_add(struct drm_crtc *crtc)
{
}

#endif

drm_ioctl_t drm_version;
drm_ioctl_t drm_getunique;
drm_ioctl_t drm_getclient;

/* drm_syncobj.c */
void drm_syncobj_open(struct drm_file *file_private);
void drm_syncobj_release(struct drm_file *file_private);
int drm_syncobj_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file_private);
int drm_syncobj_destroy_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_private);
int drm_syncobj_handle_to_fd_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file_private);
int drm_syncobj_fd_to_handle_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file_private);
int drm_syncobj_transfer_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_private);
int drm_syncobj_wait_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_private);
int drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file_private);
int drm_syncobj_reset_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_private);
int drm_syncobj_signal_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file_private);
int drm_syncobj_timeline_signal_ioctl(struct drm_device *dev, void *data,
				      struct drm_file *file_private);
int drm_syncobj_query_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_private);

/* drm_framebuffer.c */
void drm_framebuffer_print_info(struct drm_printer *p, unsigned int indent,
				const struct drm_framebuffer *fb);
int drm_framebuffer_debugfs_init(struct drm_minor *minor);

/* drm_hdcp.c */
#ifdef  __linux__
int drm_setup_hdcp_srm(struct class *drm_class);
void drm_teardown_hdcp_srm(struct class *drm_class);
#elif defined(__FreeBSD__)
static inline int
drm_setup_hdcp_srm(struct class *drm_class)
{

	return (0);
}
static inline void
drm_teardown_hdcp_srm(struct class *drm_class)
{
}
#endif
