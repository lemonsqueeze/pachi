
#ifndef PACHI_CAFFE_H
#define PACHI_CAFFE_H


bool caffe_ready(void);
void caffe_init(int size);
void caffe_get_data(float *data, float *result, int planes, int size);

#ifdef DCNN
void quiet_caffe(int argc, char *argv[]);
#else
#define quiet_caffe(argc, argv) ((void)0)
#endif


#endif /* PACHI_CAFFE_H */
