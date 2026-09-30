// macOS camera capture via AVFoundation. This is the only non-C file:
// it grabs frames and hands plain RGB bytes to the C side (see camera.h).
#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "camera.h"

static uint8_t *g_frame;
static int g_w, g_h;
static uint64_t g_seq, g_last_read;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

@interface CamDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation CamDelegate
- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection *)connection {
    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sample);
    if (!img) return;
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    int w = (int)CVPixelBufferGetWidth(img);
    int h = (int)CVPixelBufferGetHeight(img);
    size_t stride = CVPixelBufferGetBytesPerRow(img);
    const uint8_t *src = CVPixelBufferGetBaseAddress(img);

    pthread_mutex_lock(&g_lock);
    if (w != g_w || h != g_h) {
        free(g_frame);
        g_frame = malloc((size_t)w * h * 3);
        g_w = w;
        g_h = h;
    }
    if (g_frame && src) {
        for (int y = 0; y < h; y++) {
            const uint8_t *row = src + y * stride;
            uint8_t *dst = g_frame + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) { // BGRA -> RGB
                dst[x * 3 + 0] = row[x * 4 + 2];
                dst[x * 3 + 1] = row[x * 4 + 1];
                dst[x * 3 + 2] = row[x * 4 + 0];
            }
        }
        g_seq++;
    }
    pthread_mutex_unlock(&g_lock);
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}
@end

static AVCaptureSession *g_session;
static CamDelegate *g_delegate;
static dispatch_queue_t g_queue;

int cam_open(void) {
    @autoreleasepool {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (status == AVAuthorizationStatusNotDetermined) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block BOOL granted = NO;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                     completionHandler:^(BOOL ok) {
                                         granted = ok;
                                         dispatch_semaphore_signal(sem);
                                     }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            if (!granted) return CAM_ERR_DENIED;
        } else if (status != AVAuthorizationStatusAuthorized) {
            return CAM_ERR_DENIED;
        }

        AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!device) return CAM_ERR_NODEVICE;

        NSError *error = nil;
        AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
        if (!input) return CAM_ERR_OPEN;

        g_session = [[AVCaptureSession alloc] init];
        if ([g_session canSetSessionPreset:AVCaptureSessionPreset640x480])
            g_session.sessionPreset = AVCaptureSessionPreset640x480;
        if (![g_session canAddInput:input]) return CAM_ERR_OPEN;
        [g_session addInput:input];

        AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        output.alwaysDiscardsLateVideoFrames = YES;
        g_delegate = [CamDelegate new];
        g_queue = dispatch_queue_create("ascii-cam.capture", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:g_delegate queue:g_queue];
        if (![g_session canAddOutput:output]) return CAM_ERR_OPEN;
        [g_session addOutput:output];

        [g_session startRunning];
    }
    return CAM_OK;
}

int cam_copy(uint8_t **buf, size_t *cap, int *w, int *h) {
    int fresh = 0;
    pthread_mutex_lock(&g_lock);
    if (g_frame && g_seq != g_last_read) {
        size_t need = (size_t)g_w * g_h * 3;
        if (*cap < need) {
            uint8_t *grown = realloc(*buf, need);
            if (!grown) {
                pthread_mutex_unlock(&g_lock);
                return 0;
            }
            *buf = grown;
            *cap = need;
        }
        memcpy(*buf, g_frame, need);
        *w = g_w;
        *h = g_h;
        g_last_read = g_seq;
        fresh = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return fresh;
}

void cam_close(void) {
    @autoreleasepool {
        [g_session stopRunning];
        g_session = nil;
    }
}
