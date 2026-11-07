// SPDX-License-Identifier: Apache-2.0
// Offline diagnostic: no audio device is opened and no samples are played.
#include <AudioToolbox/AudioToolbox.h>
#include <array>
#include <cstdio>

int main() {
    bool failed=false;
    for(auto inputTag:{kAudioChannelLayoutTag_WAVE_5_1_B,kAudioChannelLayoutTag_MPEG_5_1_A}) {
        for(auto outputTag:{kAudioChannelLayoutTag_MPEG_5_1_A,kAudioChannelLayoutTag_WAVE_5_1_B}) {
            AudioChannelLayout input{},output{};
            input.mChannelLayoutTag=inputTag;output.mChannelLayoutTag=outputTag;
            const AudioChannelLayout *layouts[]={&input,&output};
            // AudioFormat.h: ChannelMap returns one input index per output;
            // MatrixMixMap returns input rows by output columns. These APIs
            // need not apply the same policy to distinct speaker labels.
            std::array<SInt32,6> map;map.fill(-99);
            UInt32 size=sizeof(map);
            auto status=AudioFormatGetProperty(kAudioFormatProperty_ChannelMap,
                sizeof(layouts),layouts,&size,map.data());
            std::printf("input_tag=%u output_tag=%u channel_map_status=%d size=%u\n",
                inputTag,outputTag,int(status),size);
            failed|=status!=noErr || size!=sizeof(map);
            if(!status){for(auto value:map)std::printf(" %d",int(value));std::puts("");}
            std::array<Float32,36> matrix{};size=sizeof(matrix);
            status=AudioFormatGetProperty(kAudioFormatProperty_MatrixMixMap,
                sizeof(layouts),layouts,&size,matrix.data());
            std::printf("matrix_status=%d size=%u\n",int(status),size);
            failed|=status!=noErr || size!=sizeof(matrix);
            if(!status)for(unsigned row=0;row<6;++row) {
                for(unsigned column=0;column<6;++column)std::printf(" %g",matrix[row*6+column]);
                std::puts("");
            }
        }
    }
    return failed?1:0;
}
