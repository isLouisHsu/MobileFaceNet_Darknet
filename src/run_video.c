#include "mtcnn.h"
#include "mobilefacenet.h"
#include "crop_align.h"

static int g_videoDone = 0;
static char* winname = "frame";
static CvFont font;

static CvCapture* g_cvCap = NULL;
static image g_imFrame[3];
static int g_index = 0;

static double g_time;
static double g_fps;
static int g_running = 0;
static detect* g_dets = NULL;
static int g_ndets = 0;

static params p;
static network* pnet;
static network* rnet;
static network* onet;

#define N 128

static network* mobilefacenet;
static landmark g_aligned = {0};
static int g_mode = 0;
static int g_initialized = 0;
static float* g_feat_saved = NULL;
static float* g_feat_toverify = NULL;
static float g_cosine = 0;
static float g_thresh = 0.3;
static int g_isOne = -1;

image _frame()
{
    IplImage* iplFrame = cvQueryFrame(g_cvCap);
    image dst = ipl_to_image(iplFrame); // BGR
    rgbgr_image(dst);                   // RGB
    return dst;
}

void* read_frame_in_thread(void* ptr)
{
    free_image(g_imFrame[g_index]);
    g_imFrame[g_index] = _frame();
    if (g_imFrame[g_index].data == 0){
        g_videoDone = 1;
        return 0;
    }
    return 0;
}

void* detect_frame_in_thread(void* ptr)
{
    g_running = 1;

    image frame = g_imFrame[(g_index + 2) % 3];
    g_dets = realloc(g_dets, 0); g_ndets = 0;
    detect_image(pnet, rnet, onet, frame, &g_ndets, &g_dets, p);

    g_running = 0;
}

void generate_feature(image im, bbox box, landmark mark, float* X)
{
    float* x = NULL;
    // image warped = image_crop_aligned(im, box, mark, g_aligned, H, W, g_mode);
    image warped = image_aligned_v2(im, mark, g_aligned, H, W, g_mode);
    image cvt = convert_mobilefacenet_image(warped);
    
    x = network_predict(mobilefacenet, cvt.data);
    memcpy(X,     x, N*sizeof(float));

    flip_image(cvt);
    x = network_predict(mobilefacenet, cvt.data);
    memcpy(X + N, x, N*sizeof(float));

    free_image(warped); free_image(cvt);
}

void* display_frame_in_thread(void* ptr)
{
    while(g_running);

    image im = g_imFrame[(g_index + 1) % 3];
    IplImage* iplFrame = image_to_ipl(im);
    for (int i = 0; i < g_ndets; i++ ){
        detect det = g_dets[i];
        float score = det.score;
        bbox bx = det.bx;
        landmark mk = det.mk;

        char buff[256];
        sprintf(buff, "%.2f", score);
        cvPutText(iplFrame, buff, cvPoint((int)bx.x1, (int)bx.y1),
                    &font, cvScalar(0, 0, 255, 0));

        cvRectangle(iplFrame, cvPoint((int)bx.x1, (int)bx.y1),
                    cvPoint((int)bx.x2, (int)bx.y2),
                    cvScalar(255, 255, 255, 0), 1, 8, 0);

        cvCircle(iplFrame, cvPoint((int)mk.x1, (int)mk.y1),
                    1, cvScalar(255, 255, 255, 0), 1, 8, 0);
        cvCircle(iplFrame, cvPoint((int)mk.x2, (int)mk.y2),
                    1, cvScalar(255, 255, 255, 0), 1, 8, 0);
        cvCircle(iplFrame, cvPoint((int)mk.x3, (int)mk.y3),
                    1, cvScalar(255, 255, 255, 0), 1, 8, 0);
        cvCircle(iplFrame, cvPoint((int)mk.x4, (int)mk.y4),
                    1, cvScalar(255, 255, 255, 0), 1, 8, 0);
        cvCircle(iplFrame, cvPoint((int)mk.x5, (int)mk.y5),
                    1, cvScalar(255, 255, 255, 0), 1, 8, 0);
    }
    im = ipl_to_image(iplFrame);
    cvReleaseImage(&iplFrame);

    int c = show_image(im, winname, 1);


    if (c != -1) c = c%256;
    if (c == 27) {          // Esc
        g_videoDone = 1;
        return 0;
    } else if (c == 's') {  // save feature
        im = g_imFrame[(g_index + 1) % 3];
        int idx = keep_one(g_dets, g_ndets, im);
        if (idx < 0) return 0;
        bbox box = g_dets[idx].bx; landmark mark = g_dets[idx].mk;
        generate_feature(im, box, mark, g_feat_saved);
        g_initialized = 1;
    } else if (c == 'v') {  // verify
        im = g_imFrame[(g_index + 1) % 3];
        int idx = keep_one(g_dets, g_ndets, im);
        if (idx < 0) return 0;
        bbox box = g_dets[idx].bx; landmark mark = g_dets[idx].mk;
        generate_feature(im, box, mark, g_feat_toverify);

        g_cosine = distCosine(g_feat_saved, g_feat_toverify, N*2);
        g_isOne = g_cosine < g_thresh? 0: 1;
    } else if (c == '[') {
        g_thresh -= 0.05;
    } else if (c == ']') {
        g_thresh += 0.05;
    }

    printf("\033[2J");
    printf("\033[1;1H");
    printf("\nFPS:%.1f\n", g_fps);
    printf("Objects:%d\n\n", g_ndets);
    printf("Initialized:%d\n", g_initialized);
    printf("Thresh:%.4f\n", g_thresh);
    printf("Cosine:%.4f\n", g_cosine);
    printf("Verify:%d\n", g_isOne);

    return 0;
}

int verify_video_demo(int argc, char **argv)
{
    pnet = load_mtcnn_net("PNet");
    rnet = load_mtcnn_net("RNet");
    onet = load_mtcnn_net("ONet");
    mobilefacenet = load_mobilefacenet();
    printf("\n\n");

    printf("Initializing Capture...");
    int index = find_int_arg(argc, argv, "--index", 0);
    if (index < 0){
        char* filepath = find_char_arg(argc, argv, "--path", "../images/test.mp4");
        if(0==strcmp(filepath, "../images/test.mp4")){
            fprintf(stderr, "Using default: %s\n", filepath);
        }
        g_cvCap = cvCaptureFromFile(filepath);
    } else {
        g_cvCap = cvCaptureFromCAM(index);
    }
    if (!g_cvCap){
        printf("failed!\n");
        return -1;
    }
    // cvSetCaptureProperty(g_cvCap, CV_CAP_PROP_FRAME_HEIGHT, H);
    // cvSetCaptureProperty(g_cvCap, CV_CAP_PROP_FRAME_WIDTH, W);
    
    g_imFrame[0] = _frame();
    g_imFrame[1] = copy_image(g_imFrame[0]);
    g_imFrame[2] = copy_image(g_imFrame[0]);

    cvNamedWindow(winname, CV_WINDOW_AUTOSIZE);
    cvInitFont(&font, CV_FONT_HERSHEY_SIMPLEX, 0.5, 0.5, 1, 2, 8);
    printf("OK!\n");

    printf("Initializing detection...");
    p = initParams(argc, argv);
    g_dets = calloc(0, sizeof(detect)); g_ndets = 0;
    printf("OK!\n");

    printf("Initializing verification...");
    // g_aligned = initAlignedOffset();
    g_aligned = initAligned();
    g_mode = find_int_arg(argc, argv, "--mode", 1);
    g_thresh = find_float_arg(argc, argv, "--thresh", 0.3);
    g_feat_saved = calloc(2*N, sizeof(float));
    g_feat_toverify = calloc(2*N, sizeof(float));
    printf("OK!\n");

    pthread_t thread_read;
    pthread_t thread_detect;
    pthread_t thread_display;

    g_time = what_time_is_it_now();
    while(!g_videoDone){
        g_index = (g_index + 1) % 3;

        if(pthread_create(&thread_read, 0, read_frame_in_thread, 0)) error("Thread read create failed");
        if(pthread_create(&thread_detect, 0, detect_frame_in_thread, 0)) error("Thread detect create failed");
        if(pthread_create(&thread_display, 0, display_frame_in_thread, 0)) error("Thread detect create failed");
        
        g_fps = 1./(what_time_is_it_now() - g_time);
        g_time = what_time_is_it_now();
        // display_frame_in_thread(0);
        
        pthread_join(thread_read, 0);
        pthread_join(thread_detect, 0);
        pthread_join(thread_display, 0);
    }
    for (int i = 0; i < 3; i++ ){
        free_image(g_imFrame[i]);
    }
    free(g_dets);
    cvReleaseCapture(&g_cvCap);
    cvDestroyWindow(winname);

    free_network(pnet);
    free_network(rnet);
    free_network(onet);
    free_network(mobilefacenet);
    
    free(g_feat_saved); free(g_feat_toverify);

    return 0;
}

