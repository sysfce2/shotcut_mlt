/*
 * filter_dance.c -- animate images size and position to the audio
 * Copyright (C) 2015 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <framework/mlt.h>
#include <stdlib.h> // calloc(), free()
#include <math.h>   // sin()

// Private Constants
static const int WINDOW_SIZE = 2048; // 23.43Hz FFT bins at 48kHz
static const double PI = 3.14159265358979323846;

// Private Types
typedef struct
{
	mlt_filter affine;
	mlt_filter fft;
	char* mag_prop_name;
	int rel_pos;
	double phase;
	int preprocess_warned;
} private_data;

static double apply( double positive, double negative, double mag, double max_range )
{
	if( mag == 0.0 )
	{
		return 0.0;
	}
	else if( mag > 0.0 && positive > 0.0 )
	{
		return positive * mag * max_range;
	}
	else if( mag < 0.0 && negative > 0.0 )
	{
		return negative * mag * max_range;
	}
	else if( positive )
	{
		return positive * fabs( mag ) * max_range;
	}
	else if ( negative )
	{
		return negative * -fabs( mag ) * max_range;
	}

	return 0.0;
}

static int filter_get_audio( mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples )
{
	mlt_filter filter = (mlt_filter)mlt_frame_pop_audio( frame );
	mlt_properties filter_properties = MLT_FILTER_PROPERTIES( filter );
	private_data* private = (private_data*)filter->child;
	mlt_properties fft_properties = MLT_FILTER_PROPERTIES( private->fft );
	double low_freq = mlt_properties_get_int( filter_properties, "frequency_low" );
	double hi_freq = mlt_properties_get_int( filter_properties, "frequency_high" );
	double threshold = mlt_properties_get_int( filter_properties, "threshold" );
	double osc = mlt_properties_get_int( filter_properties, "osc" );
	float peak = 0;

	// The service must stay locked while using the private data
	mlt_service_lock( MLT_FILTER_SERVICE( filter ) );

	// Perform FFT processing on the frame
	mlt_filter_process( private->fft, frame );
	mlt_frame_get_audio( frame, buffer, format, frequency, channels, samples );

	float* bins = mlt_properties_get_data( fft_properties, "bins", NULL );
	double window_level = mlt_properties_get_double( fft_properties, "window_level" );

	if( bins && window_level == 1.0 )
	{
		// Find the peak FFT magnitude in the configured range of frequencies
		int bin_count = mlt_properties_get_int( fft_properties, "bin_count" );
		double bin_width = mlt_properties_get_double( fft_properties, "bin_width" );
		int bin = 0;
		for( bin = 0; bin < bin_count; bin++ )
		{
			double F = bin_width * (double)bin;
			if( F >= low_freq && F <= hi_freq )
			{
				if( bins[bin] > peak )
				{
					peak = bins[bin];
				}
			}
		}
	}

	mlt_service_unlock( MLT_FILTER_SERVICE( filter ) );

	// Scale the magnitude to dB and apply oscillation
	double dB = peak > 0.0 ? 20 * log10( peak ) : -1000.0;
	double mag = 0.0;
	if( dB >= threshold )
	{
		// Scale to range 0.0-1.0
		mag = 1 - (dB / threshold);
		if( osc != 0 )
		{
			// Apply the oscillation
			double fps = mlt_profile_fps( mlt_service_profile( MLT_FILTER_SERVICE(filter) ) );
			double t = private->rel_pos / fps;
			mag = mag * sin( 2 * PI * osc * t + private->phase );
		}
		private->rel_pos++;
	} else {
		private->rel_pos = 1;
		// Alternate the phase so that the dancing alternates directions to the beat.
		private->phase = private->phase ? 0 : PI;
		mag = 0;
	}

	// Save the magnitude as a property on the frame to be used in get_image()
	mlt_properties_set_double( MLT_FRAME_PROPERTIES(frame), private->mag_prop_name, mag );

	return 0;
}

/** Get the image.
*/
static int filter_get_image( mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable )
{
	int error = 0;
	mlt_filter filter = (mlt_filter)mlt_frame_pop_service( frame );
	private_data* private = (private_data*)filter->child;
	mlt_properties filter_properties = MLT_FILTER_PROPERTIES( filter );
	mlt_properties frame_properties = MLT_FRAME_PROPERTIES( frame );

	if( mlt_properties_get( frame_properties, private->mag_prop_name ) )
	{
		double mag = mlt_properties_get_double( frame_properties, private->mag_prop_name );

		// scale_x and scale_y are in the range 0.0 to x.0 with:
		//    0.0 = the largest possible
		//  < 1.0 = increase size (zoom in)
		//    1.0 = no scaling
		//  > 1.0 = decrease size (zoom out)
		double initial_zoom = mlt_properties_get_double( filter_properties, "initial_zoom" );
		double zoom = mlt_properties_get_double( filter_properties, "zoom" );
		double scale_xy = (100.0 / initial_zoom ) - ( fabs(mag) * (zoom / 100.0) );
		if( scale_xy < 0.1 ) scale_xy = 0.1;

		// ox is in the range -meta.media.width to +meta.media.width with:
		//  > 0 = offset to the left
		//    0 = no offset
		//  < 0 = offset to the right
		double native_width = mlt_properties_get_int( frame_properties, "meta.media.width" );
		double left = mlt_properties_get_double( filter_properties, "left" );
		double right = mlt_properties_get_double( filter_properties, "right" );
		double ox = apply( left, right, mag, native_width / 100.0 );

		// oy is in the range -meta.media.height to +meta.media.height with:
		//  > 0 = offset up
		//    0 = no offset
		//  < 0 = offset down
		double native_height = mlt_properties_get_int( frame_properties, "meta.media.height" );
		double up = mlt_properties_get_double( filter_properties, "up" );
		double down = mlt_properties_get_double( filter_properties, "down" );
		double oy = apply( up, down, mag, native_height / 100.0 );

		// fix_rotate_x is in the range -360 to +360 with:
		// > 0 = rotate clockwise
		//   0 = no rotation
		// < 0 = rotate anticlockwise
		double counterclockwise = mlt_properties_get_double( filter_properties, "counterclockwise" );
		double clockwise = mlt_properties_get_double( filter_properties, "clockwise" );
		double fix_rotate_x = apply( clockwise, counterclockwise, mag, 1.0 );

		mlt_service_lock( MLT_FILTER_SERVICE( filter ) );
		mlt_properties affine_properties = MLT_FILTER_PROPERTIES( private->affine );
		mlt_properties_set_double( affine_properties, "transition.scale_x", scale_xy );
		mlt_properties_set_double( affine_properties, "transition.scale_y", scale_xy );
		mlt_properties_set_double( affine_properties, "transition.ox", ox );
		mlt_properties_set_double( affine_properties, "transition.oy", oy );
		mlt_properties_set_double( affine_properties, "transition.fix_rotate_x", fix_rotate_x );
		mlt_filter_process( private->affine, frame );
		error = mlt_frame_get_image( frame, image, format, width, height, 0 );
		mlt_service_unlock( MLT_FILTER_SERVICE( filter ) );
	} else if ( private->preprocess_warned++ == 2 ) {
		// This filter depends on the consumer processing the audio before the
		// video.
		mlt_log_warning( MLT_FILTER_SERVICE(filter), "Audio not preprocessed. Unable to dance.\n" );
	}

	return error;
}

/** Filter processing.
*/
static mlt_frame filter_process( mlt_filter filter, mlt_frame frame )
{
	mlt_frame_push_audio( frame, filter );
	mlt_frame_push_audio( frame, filter_get_audio );
	mlt_frame_push_service( frame, filter );
	mlt_frame_push_get_image( frame, filter_get_image );
	return frame;
}

static void filter_close( mlt_filter filter )
{
	private_data* private = (private_data*)filter->child;

	if ( private )
	{
		mlt_filter_close( private->affine );
		mlt_filter_close( private->fft );
		free( private->mag_prop_name );
		free( private );
	}
	filter->child = NULL;
	filter->close = NULL;
	filter->parent.close = NULL;
	mlt_service_close( &filter->parent );
}

/** Constructor for the filter.
*/
mlt_filter filter_dance_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	mlt_filter filter = mlt_filter_new();
	private_data* private = (private_data*)calloc( 1, sizeof(private_data) );
	mlt_filter affine_filter = mlt_factory_filter( profile, "affine", NULL );
	mlt_filter fft_filter = mlt_factory_filter( profile, "fft", NULL );

	if ( filter && private && affine_filter && fft_filter )
	{
		mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
		mlt_properties_set_int( properties, "_filter_private", 1 );
		mlt_properties_set_int( properties, "frequency_low", 20 );
		mlt_properties_set_int( properties, "frequency_high", 20000 );
		mlt_properties_set_double( properties, "threshold", -30.0 );
		mlt_properties_set_double( properties, "osc", 5.0 );
		mlt_properties_set_double( properties, "initial_zoom", 100.0 );
		mlt_properties_set_double( properties, "zoom", 10.0 );
		mlt_properties_set_double( properties, "left", 0.0 );
		mlt_properties_set_double( properties, "right", 0.0 );
		mlt_properties_set_double( properties, "up", 0.0 );
		mlt_properties_set_double( properties, "down", 0.0 );
		mlt_properties_set_double( properties, "clockwise", 0.0 );
		mlt_properties_set_double( properties, "counterclockwise", 0.0 );

		// Configure FFT filter
		mlt_properties_set_int( MLT_FILTER_PROPERTIES( fft_filter ), "window_size", WINDOW_SIZE );

		// Create a unique ID for storing data on the frame
		const char* unique_id = mlt_properties_get( MLT_FILTER_PROPERTIES( fft_filter ), "_unique_id" );
		private->mag_prop_name = calloc( 1, 20 );
		snprintf( private->mag_prop_name, 20, "fft_mag.%s", unique_id );
		private->mag_prop_name[20 - 1] = '\0';

		private->affine = affine_filter;
		private->fft = fft_filter;

		filter->close = filter_close;
		filter->process = filter_process;
		filter->child = private;
	}
	else
	{
		mlt_log_error( MLT_FILTER_SERVICE(filter), "Filter dance failed\n" );

		if( filter )
		{
			mlt_filter_close( filter );
		}

		if( affine_filter )
		{
			mlt_filter_close( affine_filter );
		}

		if( fft_filter )
		{
			mlt_filter_close( fft_filter );
		}

		if( private )
		{
			free( private );
		}

		filter = NULL;
	}
	return filter;
}