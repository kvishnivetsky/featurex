/**
 * @file		melFB.cpp
 * @date		Mar 30, 2011
 * @author  	Gary Grutzek
 * @brief		Mel filter bank
 * @details		Mel filter bank,
 * 	   			logarithmic filterbank, mel-style but without linear spacing,
 * 	   			log spacing from lowest to highest freq
 *
 * @copyright  	Copyright (c) 2012 Gary Grutzek<br>
 * 				Cologne University of Applied Sciences<br>
 * 				<br>
 * 				This program is free software: you can redistribute it and/or modify
 *				it under the terms of the GNU General Public License as published by
 *				the Free Software Foundation, either version 3 of the License, or
 * 				(at your option) any later version.<br>
 *				This program is distributed in the hope that it will be useful,
 *				but WITHOUT ANY WARRANTY; without even the implied warranty of
 *				MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *				GNU General Public License for more details.<br>
 *				You should have received a copy of the GNU General Public License
 *				along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "melFB.h"
#include <cmath>
#include <iostream>

#define SAMPLE_RATE 8000.0f
#ifndef __APPLE__
#define logf log
#define roundf round
#define powf pow
#endif


MelFb::MelFb(int fftSize, int numBanks, int locut, int hicut)
{
    numFilterBanks = numBanks;

    // calc next power of 2
    int fftOrder = (int)ceil(logf((float)fftSize)/logf(2.0f));
    fbSize = (1 << fftOrder)/2;

    hiCut = hicut;
    if (hiCut > SAMPLE_RATE)
        throw "HiCut Parameter too high";

    loCut = locut;
    fbank = NULL;

    if (numFilterBanks == 0)
        throw "Number of Filterbanks is zero";

    calcMelFB();
}

MelFb::~MelFb()
{
    if (fbank != NULL)
    {
		for (int j = 0; j < numFilterBanks; j++)
			delete [] fbank[j] ;
		delete [] fbank;
	}
    fbank = NULL;
    fwsFree(fLo);
    fwsFree(fHi);

}


void MelFb::calcMelFB()
{

    // size of frequency bins in Hz => fs/fftsize
    Fw32f binSize = SAMPLE_RATE/(fbSize*2.0f-1);
    // centerfrequncy of bands
    Fw32f *fCenter = fwsMalloc_32f(numFilterBanks);
    // lower bounds of bands
    fLo = fwsMalloc_32f(numFilterBanks);
    // higher bounds of bands
    fHi = fwsMalloc_32f(numFilterBanks);
    // mel-equidistant delta between centerfrequencies
    Fw32f delta = (Fw32f)(linToMel(hiCut) - linToMel(loCut)) / (numFilterBanks + 1);

    // multidimensional array for filterbank
    fbank = new Fw32f *[numFilterBanks]; // alloc some pointers ...
    for (int i = 0; i < numFilterBanks ; i++)
        fbank[i] = new Fw32f[fbSize]; // ... and alloc an array for each pointer

    // get mel equidistant center frequencies
    Fw32f melCenter = linToMel(loCut);
    fLo[0] = roundf(loCut/binSize);

    for (int i=0; i<numFilterBanks; i++)
    {
        melCenter += delta;
        // calc center frequency and map to next fft bin
        fCenter[i] = roundf(melToLin(melCenter)/binSize);

        // set lower and higher bounds of neighbour bands
        if (i > 0)
            fHi[i-1] = fCenter[i];

        if (i < numFilterBanks-1)
            fLo[i+1] = fCenter[i];
    }
    fHi[numFilterBanks-1] = hiCut/binSize;

    // calc triangular filter for each band
    Fw32f x=0;
    for (int band=0; band<numFilterBanks; band++)
    {
        for (int k=0; k<fbSize; k++)
        {
            if (k > fLo[band] && k <= fCenter[band]) {
                fbank[band][k] = x;
                x += 1/(fCenter[band] - fLo[band]);
            }
            else if (k > fCenter[band] && k <= fHi[band]) {
                fbank[band][k] = x;
                x -= 1/(fHi[band] - fCenter[band]);
            }
            else
                fbank[band][k] = 0;
        }
        x = 0;
    }

    // free memory
    fwsFree(fCenter);

}


void MelFb::applyMelFB(Fw32f **output, const Stft& stft)
{
    // the non-sparse way:
    //   IppStatus error = ippStsNoErr;
    //    Ipp32f *filter = ippsMalloc_32f(fbSize);
    //    Ipp32f *frame = ippsMalloc_32f(stft.fftLen/2);
    //
    //    for (int window=0; window < stft.NoOfWindows; window++) {
    //        // copy window to frame-buffer
    //        ippsCopy_32f(stft.spectrogramm[window], frame, stft.fftLen/2);
    //
    //        for (int band=0; band<numFilterBanks; band++) {
    //            ippsCopy_32f(&fbank[band][0], filter, fbSize);
    //            // multiply spectrum with filterbank
    //            ippsMul_32f_I(frame, filter, fbSize);
    //            //  cout << "band: " << band << " window: " << window << endl;
    //            // sum
    //            ippsSum_32f(filter, fbSize, &output[window][band], ippAlgHintFast);
    //        }
    //    }

    // the "sparse" way is about 3-4x faster
    int maxFilterSize = (int)(ceil(fHi[numFilterBanks-1]) - floor(fLo[numFilterBanks-1]));
    Fw32f *filterSparse = fwsMalloc_32f(maxFilterSize);
    Fw32f *frameSparse = fwsMalloc_32f(maxFilterSize);

    int filterSize = 0;
    int loF = 0;

    for (int window=0; window < stft.NoOfWindows; window++) {

        for (int band=0; band<numFilterBanks; band++) {

            filterSize = (int) (ceil(fHi[band]) - floor(fLo[band]));
            loF = (int)floor(fLo[band]);
            fwsZero_32f(filterSparse, maxFilterSize);
            fwsZero_32f(frameSparse, maxFilterSize);

            // copy window to frame-buffer
            fwsCopy_32f(&stft.spectrogramm[window][loF], frameSparse, filterSize);
            // copy "sparse" filter from lo freq to hi freq
            fwsCopy_32f(&fbank[band][loF], filterSparse, filterSize);
            // multiply spectrum with sparse filterbank
            fwsMul_32f_I(frameSparse, filterSparse, filterSize);
            // sum band
            fwsSum_32f(filterSparse, filterSize, &output[window][band], fwAlgHintFast);
        }
    }
    fwsFree(filterSparse);
    fwsFree(frameSparse);
    //ippsFree(filter);
    //ippsFree(frame);
    //    if (error)
    //        throw "applyMelFB failed";
}


#pragma mark helper functions

Fw32f MelFb::linToMel(Fw32f linFreq)
{
    return (2595.0f * (logf(1.0f + linFreq / 700.0f) / logf(10.0f)));
}

Fw32f MelFb::melToLin(Fw32f melFreq)
{
    return (700.0f * (powf(10.0f, (melFreq / 2595.0f)) - 1.0f));
}




