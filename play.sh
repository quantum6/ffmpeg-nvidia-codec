
rm test

gcc h264_decode_min.cpp \
    -o test \
    -I/usr/local/include \
    -L/usr/local/lib -lavfilter -lavcodec -lavformat -lavutil 

./test

