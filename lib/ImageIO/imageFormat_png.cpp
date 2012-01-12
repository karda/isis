#include <DataStorage/io_interface.h>
#include <png.h>
#include <stdio.h>

namespace isis
{
namespace image_io
{

class ImageFormat_png: public FileFormat
{
protected:
	std::string suffixes( io_modes /*modes = both */)const {
			return std::string( ".png");
	}
	struct Reader{
		virtual data::Chunk operator()(png_structp png_ptr,png_infop info_ptr)const=0;
	};
	template<typename TYPE> struct GenericReader:Reader{
		data::Chunk operator()(png_structp png_ptr,png_infop info_ptr)const{
			data::Chunk ret= data::MemChunk<TYPE >( info_ptr->width, info_ptr->height );

			/* png needs a pointer to each row */
			png_bytep *row_pointers = ( png_bytep * ) malloc( sizeof( png_bytep ) * info_ptr->height );

			for ( unsigned short r = 0; r < info_ptr->height; r++ )
				row_pointers[r] = ( png_bytep )&ret.voxel<TYPE>( 0, r );

			png_read_image( png_ptr, row_pointers );
			return ret;
		}
	};
	std::map<png_byte,std::map<png_byte,boost::shared_ptr<Reader> > > readers;
public:
	ImageFormat_png(){
		readers[PNG_COLOR_TYPE_GRAY][8].reset(new GenericReader<uint8_t>);
		readers[PNG_COLOR_TYPE_GRAY][16].reset(new GenericReader<uint16_t>);
		readers[PNG_COLOR_TYPE_RGB][8].reset(new GenericReader<util::color24>);
		readers[PNG_COLOR_TYPE_RGB][16].reset(new GenericReader<util::color48>);
	}
	std::string getName()const {
		return "PNG (Portable Network Graphics)";
	}
	std::string dialects( const std::string &/*filename*/ ) const {
		return "middle";
	}
	bool write_png( const std::string &filename, const data::Chunk &buff ) {
		FILE *fp;
		png_structp png_ptr;
		png_infop info_ptr;
		assert( buff.getRelevantDims() == 2 );
		util::FixedVector<size_t, 4> size = buff.getSizeAsVector();

		/* open the file */
		fp = fopen( filename.c_str(), "wb" );

		if ( fp == NULL ) {
			throwSystemError( errno, std::string( "Failed to open " ) + filename );
			return 0;
		}

		/* Create and initialize the png_struct with the desired error handler
		* functions.  If you want to use the default stderr and longjump method,
		* you can supply NULL for the last three parameters.  We also check that
		* the library version is compatible with the one used at compile time,
		* in case we are using dynamically linked libraries.  REQUIRED.
		*/
		png_ptr = png_create_write_struct( PNG_LIBPNG_VER_STRING, NULL /*user_error_ptr*/, NULL /*user_error_fn*/, NULL /*user_warning_fn*/ );

		if ( png_ptr == NULL ) {
			fclose( fp );
			throwSystemError( errno, "png_create_write_struct failed" );
			return 0;
		}

		/* Allocate/initialize the image information data.  REQUIRED */
		info_ptr = png_create_info_struct( png_ptr );

		if ( info_ptr == NULL ) {
			fclose( fp );
			throwSystemError( errno, "png_create_info_struct failed" );
			return 0;
		}

		/* Set error handling.  REQUIRED if you aren't supplying your own
		* error handling functions in the png_create_write_struct() call.
		*/
		if ( setjmp( png_jmpbuf( png_ptr ) ) ) {
			/* If we get here, we had a problem writing the file */
			fclose( fp );
			png_destroy_write_struct( &png_ptr, &info_ptr );
			throwSystemError( errno, std::string( "Could not write to " ) + filename );
			return false;
		}

		/* set up the output control if you are using standard C streams */
		png_init_io( png_ptr, fp );
		png_set_IHDR( png_ptr, info_ptr, size[0], size[1], 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT );

		/* png needs a pointer to each row */
		png_byte **row_pointers = new png_byte*[size[1]];

		for ( unsigned short r = 0; r < size[1]; r++ )
			row_pointers[r] = ( png_byte * )&buff.voxel<png_byte>( 0, r );

		png_set_rows( png_ptr, info_ptr, row_pointers );

		/* This is the easy way.  Use it if you already have all the
		* image info living info in the structure.  You could "|" many
		* PNG_TRANSFORM flags into the png_transforms integer here.
		*/
		png_write_png( png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL );

		/* clean up after the write, and free any memory allocated */
		png_destroy_write_struct( &png_ptr, &info_ptr );
		delete[] row_pointers;

		/* close the file */
		fclose( fp );

		/* that's it */
		return true;
	}

	data::Chunk read_png( const std::string &filename ) {
		png_byte header[8]; // 8 is the maximum size that can be checked

		/* open file and test for it being a png */
		FILE *fp = fopen( filename.c_str(), "rb" );

		if ( !fp ) {
			throwSystemError( errno, std::string( "Could not open " ) + filename );
		}

		if(fread( header, 1, 8, fp )!=8){
			throwSystemError( errno, std::string( "Could not open " ) + filename );
		}
			
		;

		if ( png_sig_cmp( header, 0, 8 ) ) {
			throwGenericError( filename + " is not recognized as a PNG file" );
		}

		png_structp png_ptr;
		png_infop info_ptr;

		/* initialize stuff */
		png_ptr = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
		assert( png_ptr );

		info_ptr = png_create_info_struct( png_ptr );
		assert( info_ptr );

		png_init_io( png_ptr, fp );
		png_set_sig_bytes( png_ptr, 8 );
		png_read_info( png_ptr, info_ptr );
		png_set_interlace_handling( png_ptr );
		png_read_update_info( png_ptr, info_ptr );
		
		LOG( Debug, info ) << "color_type " << ( int )info_ptr->color_type << " bit_depth " << ( int )info_ptr->bit_depth;

		boost::shared_ptr< Reader > reader = readers[info_ptr->color_type][info_ptr->bit_depth];
		if(!reader){
			LOG( Runtime, error) << "Sorry, the color type " << ( int )info_ptr->color_type << " with " << ( int )info_ptr->bit_depth << " bits is not supportet.";
			throwGenericError("Wrong color type");
		}

		data::Chunk ret = (*reader)(png_ptr,info_ptr);

		fclose( fp );
		LOG( Runtime, notice ) << ret.getSizeAsString() << "-image loaded from png. Making up acquisitionNumber,columnVec,indexOrigin,rowVec and voxelSize";
		ret.setPropertyAs<uint32_t>( "acquisitionNumber", 0 );
		ret.setPropertyAs<util::fvector4>( "rowVec", util::fvector4( 1, 0 ) );
		ret.setPropertyAs<util::fvector4>( "columnVec", util::fvector4( 0, 1 ) );
		ret.setPropertyAs<util::fvector4>( "indexOrigin", util::fvector4( 0, 0 ) );
		ret.setPropertyAs<util::fvector4>( "voxelSize", util::fvector4( 1, 1, 1 ) );
		return ret;
	}
	int load ( std::list<data::Chunk> &chunks, const std::string &filename, const std::string &/*dialect*/ )  throw( std::runtime_error & ) {
		chunks.push_back( read_png( filename ) );
		return 0;
	}

	void write( const data::Image &image, const std::string &filename, const std::string &dialect )  throw( std::runtime_error & ) {
		if( image.getRelevantDims() < 2 ) {
			throwGenericError( "Cannot write png when image is made of stripes" );
		}

		data::TypedImage<png_byte> tImg( image );
		tImg.spliceDownTo( data::sliceDim );
		std::vector<data::Chunk > chunks = tImg.copyChunksToVector( false );
		unsigned short numLen = std::log10( chunks.size() ) + 1;
		size_t number = 0;

		if( util::istring( dialect.c_str() ) == util::istring( "middle" ) ) { //save only the middle
			LOG( Runtime, info ) << "Writing the slice " << chunks.size() / 2 + 1 << " of " << chunks.size() << " slices as png-image of size " << chunks.front().getSizeAsString();

			if( !write_png( filename, chunks[chunks.size() / 2] ) ) {
				throwGenericError( std::string( "Failed to write " ) + filename );
			}
		} else { //save all slices
			const std::pair<std::string, std::string> fname = makeBasename( filename );
			LOG( Runtime, info )
					<< "Writing " << chunks.size() << " slices as png-images " << fname.first << "_"
					<< std::string( numLen, 'X' ) << fname.second << " of size " << chunks.front().getSizeAsString();

			BOOST_FOREACH( const data::Chunk & ref, chunks ) {
				const std::string num = boost::lexical_cast<std::string>( ++number );
				const std::string name = fname.first + "_" + std::string( numLen - num.length(), '0' ) + num + fname.second;

				if( !write_png( name, ref ) ) {
					throwGenericError( std::string( "Failed to write " ) + name );;
				}
			}
		}

	}
	bool tainted()const {return false;}//internal plugins are not tainted
};
}
}
isis::image_io::FileFormat *factory()
{
	return new isis::image_io::ImageFormat_png();
}
