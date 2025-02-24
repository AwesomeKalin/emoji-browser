// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/vr/arcore_device/arcore_java_utils.h"

#include "base/android/jni_string.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/android/vr/arcore_device/arcore_device_provider.h"
#include "chrome/browser/android/vr/arcore_device/arcore_shim.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "device/vr/android/arcore/arcore_device_provider_factory.h"
#include "jni/ArCoreJavaUtils_jni.h"

using base::android::AttachCurrentThread;
using base::android::ScopedJavaLocalRef;

namespace vr {

namespace {

class ArCoreDeviceProviderFactoryImpl
    : public device::ArCoreDeviceProviderFactory {
 public:
  ArCoreDeviceProviderFactoryImpl() = default;
  ~ArCoreDeviceProviderFactoryImpl() override = default;
  std::unique_ptr<device::VRDeviceProvider> CreateDeviceProvider() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(ArCoreDeviceProviderFactoryImpl);
};

std::unique_ptr<device::VRDeviceProvider>
ArCoreDeviceProviderFactoryImpl::CreateDeviceProvider() {
  return std::make_unique<device::ArCoreDeviceProvider>();
}

}  // namespace

ArCoreJavaUtils::ArCoreJavaUtils(
    base::RepeatingCallback<void(bool)> ar_module_installation_callback,
    base::RepeatingCallback<void(bool)> ar_core_installation_callback)
    : ar_module_installation_callback_(ar_module_installation_callback),
      ar_core_installation_callback_(ar_core_installation_callback) {
  JNIEnv* env = AttachCurrentThread();
  if (!env)
    return;
  ScopedJavaLocalRef<jobject> j_arcore_java_utils =
      Java_ArCoreJavaUtils_create(env, (jlong)this);
  if (j_arcore_java_utils.is_null())
    return;
  j_arcore_java_utils_.Reset(j_arcore_java_utils);
}

ArCoreJavaUtils::~ArCoreJavaUtils() {
  JNIEnv* env = AttachCurrentThread();
  Java_ArCoreJavaUtils_onNativeDestroy(env, j_arcore_java_utils_);
}

void ArCoreJavaUtils::OnRequestInstallSupportedArCoreResult(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& obj,
    bool success) {
  ar_core_installation_callback_.Run(success);
}

bool ArCoreJavaUtils::CanRequestInstallArModule() {
  return Java_ArCoreJavaUtils_canRequestInstallArModule(AttachCurrentThread(),
                                                        j_arcore_java_utils_);
}

bool ArCoreJavaUtils::ShouldRequestInstallArModule() {
  return Java_ArCoreJavaUtils_shouldRequestInstallArModule(
      AttachCurrentThread(), j_arcore_java_utils_);
}

void ArCoreJavaUtils::RequestInstallArModule(int render_process_id,
                                             int render_frame_id) {
  Java_ArCoreJavaUtils_requestInstallArModule(
      AttachCurrentThread(), j_arcore_java_utils_,
      getTabFromRenderer(render_process_id, render_frame_id));
}

bool ArCoreJavaUtils::ShouldRequestInstallSupportedArCore() {
  JNIEnv* env = AttachCurrentThread();
  return Java_ArCoreJavaUtils_shouldRequestInstallSupportedArCore(
      env, j_arcore_java_utils_);
}

void ArCoreJavaUtils::RequestInstallSupportedArCore(int render_process_id,
                                                    int render_frame_id) {
  DCHECK(ShouldRequestInstallSupportedArCore());

  JNIEnv* env = AttachCurrentThread();
  Java_ArCoreJavaUtils_requestInstallSupportedArCore(
      env, j_arcore_java_utils_,
      getTabFromRenderer(render_process_id, render_frame_id));
}

void ArCoreJavaUtils::RequestArSession(
    int render_process_id,
    int render_frame_id,
    SurfaceReadyCallback ready_callback,
    SurfaceTouchCallback touch_callback,
    SurfaceDestroyedCallback destroyed_callback) {
  DVLOG(1) << __func__;
  JNIEnv* env = AttachCurrentThread();

  Java_ArCoreJavaUtils_launchArConsentDialog(
      env, j_arcore_java_utils_,
      getTabFromRenderer(render_process_id, render_frame_id));

  surface_ready_callback_ = std::move(ready_callback);
  surface_touch_callback_ = std::move(touch_callback);
  surface_destroyed_callback_ = std::move(destroyed_callback);
}

void ArCoreJavaUtils::DestroyDrawingSurface() {
  DVLOG(1) << __func__;
  JNIEnv* env = AttachCurrentThread();

  Java_ArCoreJavaUtils_destroyArImmersiveOverlay(env, j_arcore_java_utils_);
}

void ArCoreJavaUtils::OnDrawingSurfaceReady(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& obj,
    const base::android::JavaParamRef<jobject>& surface,
    int rotation,
    int width,
    int height) {
  DVLOG(1) << __func__ << ": width=" << width << " height=" << height
           << " rotation=" << rotation;
  gfx::AcceleratedWidget window =
      ANativeWindow_fromSurface(base::android::AttachCurrentThread(), surface);
  display::Display::Rotation display_rotation =
      static_cast<display::Display::Rotation>(rotation);
  surface_ready_callback_.Run(window, display_rotation, {width, height});
}

void ArCoreJavaUtils::OnDrawingSurfaceTouch(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& obj,
    bool touching,
    float x,
    float y) {
  DVLOG(3) << __func__ << ": touching=" << touching;
  surface_touch_callback_.Run(touching, {x, y});
}

void ArCoreJavaUtils::OnDrawingSurfaceDestroyed(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& obj) {
  DVLOG(1) << __func__ << ":::";
  if (surface_destroyed_callback_) {
    std::move(surface_destroyed_callback_).Run();
  }
}

void ArCoreJavaUtils::OnRequestInstallArModuleResult(
    JNIEnv* env,
    const base::android::JavaParamRef<jobject>& obj,
    bool success) {
  ar_module_installation_callback_.Run(success);
}

bool ArCoreJavaUtils::EnsureLoaded() {
  DCHECK(vr::IsArCoreSupported());

  JNIEnv* env = AttachCurrentThread();

  // TODO(crbug.com/884780): Allow loading the ARCore shim by name instead of by
  // absolute path.
  ScopedJavaLocalRef<jstring> java_path =
      Java_ArCoreJavaUtils_getArCoreShimLibraryPath(env);
  return LoadArCoreSdk(base::android::ConvertJavaStringToUTF8(env, java_path));
}

ScopedJavaLocalRef<jobject> ArCoreJavaUtils::GetApplicationContext() {
  JNIEnv* env = AttachCurrentThread();
  return Java_ArCoreJavaUtils_getApplicationContext(env);
}

base::android::ScopedJavaLocalRef<jobject> ArCoreJavaUtils::getTabFromRenderer(
    int render_process_id,
    int render_frame_id) {
  content::RenderFrameHost* render_frame_host =
      content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  DCHECK(render_frame_host);

  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(render_frame_host);
  DCHECK(web_contents);

  TabAndroid* tab_android = TabAndroid::FromWebContents(web_contents);
  DCHECK(tab_android);

  base::android::ScopedJavaLocalRef<jobject> j_tab_android =
      tab_android->GetJavaObject();
  DCHECK(!j_tab_android.is_null());

  return j_tab_android;
}

static void JNI_ArCoreJavaUtils_InstallArCoreDeviceProviderFactory(
    JNIEnv* env) {
  device::ArCoreDeviceProviderFactory::Install(
      std::make_unique<ArCoreDeviceProviderFactoryImpl>());
}

}  // namespace vr
