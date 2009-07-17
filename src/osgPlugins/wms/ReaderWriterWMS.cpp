/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/Mercator>
#include <osgEarth/TileSource>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgEarth/Registry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <sstream>
#include <stdlib.h>
#include <iomanip>

#include "Capabilities"
#include "TileService"

using namespace osgEarth;

#define PROPERTY_URL              "url"
#define PROPERTY_CAPABILITIES_URL "capabilities_url"
#define PROPERTY_TILESERVICE_URL  "tileservice_url"
#define PROPERTY_LAYERS           "layers"
#define PROPERTY_STYLE            "style"
#define PROPERTY_FORMAT           "format"
#define PROPERTY_WMS_FORMAT       "wms_format"
#define PROPERTY_TILE_SIZE        "tile_size"
#define PROPERTY_ELEVATION_UNIT   "elevation_unit"
#define PROPERTY_SRS              "srs"
#define PROPERTY_DEFAULT_TILE_SIZE "default_tile_size"

class WMSSource : public TileSource
{
public:
	WMSSource( const osgDB::ReaderWriter::Options* options ):
    TileSource( options ),
    _tile_size(256)
    {
        if ( options->getPluginData( PROPERTY_URL ) )
            _prefix = std::string( (const char*)options->getPluginData( PROPERTY_URL ) );

        if ( options->getPluginData( PROPERTY_LAYERS ) )
            _layers = std::string( (const char*)options->getPluginData( PROPERTY_LAYERS ) );

        if ( options->getPluginData( PROPERTY_STYLE ) )
            _style = std::string( (const char*)options->getPluginData( PROPERTY_STYLE ) );

        if ( options->getPluginData( PROPERTY_FORMAT ) )
            _format = std::string( (const char*)options->getPluginData( PROPERTY_FORMAT ) );

        if ( options->getPluginData( PROPERTY_WMS_FORMAT ) )
            _wms_format = std::string( (const char*)options->getPluginData( PROPERTY_WMS_FORMAT ) );

        if ( options->getPluginData( PROPERTY_CAPABILITIES_URL ) )
            _capabilitiesURL = std::string( (const char*)options->getPluginData( PROPERTY_CAPABILITIES_URL ) );

        if ( options->getPluginData( PROPERTY_TILESERVICE_URL ) )
            _tileServiceURL = std::string( (const char*)options->getPluginData( PROPERTY_TILESERVICE_URL ) );

        if ( options->getPluginData( PROPERTY_ELEVATION_UNIT))
            _elevation_unit = std::string( (const char*)options->getPluginData( PROPERTY_ELEVATION_UNIT ) );

        //Try to read the tile size
        if ( options->getPluginData( PROPERTY_TILE_SIZE ) )
        {
            _tile_size = as<int>( (const char*)options->getPluginData( PROPERTY_TILE_SIZE ), 256 );
        }
        //If the tile size wasn't specified, use the default tile size if it was specified
        else if ( options->getPluginData( PROPERTY_DEFAULT_TILE_SIZE ) )
        {
            _tile_size = as<int>( (const char*)options->getPluginData( PROPERTY_DEFAULT_TILE_SIZE ), 256 );
        }

        if ( options->getPluginData( PROPERTY_SRS ) )
            _srs = std::string( (const char*)options->getPluginData( PROPERTY_SRS ) );

        if ( _elevation_unit.empty())
            _elevation_unit = "m";
    }

    /** override */
    const Profile* createProfile( const Profile* mapProfile, const std::string& configPath )
    {
        osg::ref_ptr<const Profile> result;

        char sep = _prefix.find_first_of('?') == std::string::npos? '?' : '&';

        if ( _capabilitiesURL.empty() )
            _capabilitiesURL = _prefix + sep + "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetCapabilities";

        //Try to read the WMS capabilities
        osg::ref_ptr<Capabilities> capabilities = CapabilitiesReader::read(_capabilitiesURL);
        if ( !capabilities.valid() )
        {
            osg::notify(osg::WARN) << "[osgEarth::WMS] Unable to read WMS GetCapabilities; failing." << std::endl;
            return NULL;
        }

        osg::notify(osg::INFO) << "[osgEarth::WMS] Got capabilities from " << _capabilitiesURL << std::endl;
        if (_format.empty())
        {
            _format = capabilities->suggestExtension();
            osg::notify(osg::NOTICE) << "[osgEarth::WMS] No format specified, capabilities suggested extension " << _format << std::endl;
        }

        if ( _format.empty() )
            _format = "png";
       
        if ( _srs.empty() )
            _srs = "EPSG:4326";

        //Initialize the WMS request prototype
        std::stringstream buf;
        buf
            << std::fixed << _prefix << sep
            << "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap"
            << "&LAYERS=" << _layers
            << "&FORMAT=" << (_wms_format.empty()? "image/" + _format : _wms_format)
            << "&STYLES=" << _style
            << "&SRS=" << _srs
            << "&WIDTH="<< _tile_size
            << "&HEIGHT="<< _tile_size
            << "&BBOX=%lf,%lf,%lf,%lf";
        _prototype = buf.str();

        // first check whether the map + WMS source are the same SRS:
        // TODO: deprecate this once we start using native profiles.
        osg::ref_ptr<SpatialReference> wms_srs = SpatialReference::create( _srs );
        if ( wms_srs.valid() && mapProfile && mapProfile->getSRS()->isEquivalentTo( wms_srs.get() ) )
        {
            result = mapProfile;
        }

        // Next, try to glean the extents from the layer list
        if ( !result.valid() )
        {
            //TODO: "layers" mights be a comma-separated list. need to loop through and
            //combine the extents?? yes
            Layer* layer = capabilities->getLayerByName( _layers );
            if ( layer )
            {
                double minx, miny, maxx, maxy;
                layer->getExtents(minx, miny, maxx, maxy);

                //Check to see if the profile is equivalent to global-geodetic
                if (wms_srs->isGeographic())
                {
                  osg::ref_ptr<const Profile> globalGeodetic = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
                  if (minx == globalGeodetic->getExtent().xMin() &&
                      miny == globalGeodetic->getExtent().yMin() &&
                      maxx == globalGeodetic->getExtent().xMax() &&
                      maxy == globalGeodetic->getExtent().yMax())
                  {
                      //They are equivalent so just use the global geodetic extent
                      result = globalGeodetic.get();
                  }
                }

                if (!result.valid())
                {
                    result = Profile::create( _srs, minx, miny, maxx, maxy );
                }
            }
        }

        // Last resort: create a global extent profile (only valid for global maps)
        if ( !result.valid() && wms_srs->isGeographic() && mapProfile && mapProfile->getProfileType() != Profile::TYPE_LOCAL )
        {
            result = mapProfile;
        }
        

        // JPL uses an experimental interface called TileService -- ping to see if that's what
        // we are trying to read:
        if (_tileServiceURL.empty())
            _tileServiceURL = _prefix + sep + "request=GetTileService";

        osg::notify(osg::INFO) << "[osgEarth::WMS] Testing for JPL/TileService at " << _tileServiceURL << std::endl;
        _tileService = TileServiceReader::read(_tileServiceURL);
        if (_tileService.valid())
        {
            osg::notify(osg::NOTICE) << "[osgEarth::WMS] Found JPL/TileService spec" << std::endl;
            TileService::TilePatternList patterns;
            _tileService->getMatchingPatterns(_layers, _format, _style, _srs, _tile_size, _tile_size, patterns);

            if (patterns.size() > 0)
            {
                result = _tileService->createProfile( patterns );
                _prototype = _prefix + sep + patterns[0].getPrototype();
            }
        }
        else
        {
            osg::notify(osg::INFO) << "[osgEarth::WMS] No JPL/TileService spec found; assuming standard WMS" << std::endl;
        }

        //TODO: won't need this for OSG 2.9+, b/c of mime-type support
        _prototype = _prototype + "&." + _format;

        // populate the data metadata:
        // TODO


        return result.release();
    }

    /** override */
    osg::Image* createImage( const TileKey* key )
    {
        return osgDB::readImageFile( createURI( key ), getOptions() );
    }

    /** override */
    osg::HeightField* createHeightField( const TileKey* key )
    {
        osg::Image* image = createImage(key);
        if (!image)
        {
            osg::notify(osg::INFO) << "[osgEarth::WMS] Failed to read heightfield from " << createURI(key) << std::endl;
        }

        float scaleFactor = 1;

        //Scale the heightfield to meters
        if (_elevation_unit == "ft")
        {
            scaleFactor = 0.3048;
        }

        ImageToHeightFieldConverter conv;
        return conv.convert( image, scaleFactor );
    }

    std::string createURI( const TileKey* key ) const
    {
        double minx, miny, maxx, maxy;
        key->getGeoExtent().getBounds( minx, miny, maxx, maxy);
        // http://labs.metacarta.com/wms-c/Basic.py?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=basic&BBOX=-180,-90,0,90
        char buf[2048];
        sprintf(buf, _prototype.c_str(), minx, miny, maxx, maxy);
        return buf;
    }

    virtual int getPixelsPerTile() const
    {
        return _tile_size;
    }

    virtual std::string getExtension()  const 
    {
        return _format;
    }

private:
    std::string _prefix;
    std::string _layers;
    std::string _style;
    std::string _format;
    std::string _wms_format;
    std::string _srs;
    std::string _tileServiceURL;
    std::string _capabilitiesURL;
	int _tile_size;
    std::string _elevation_unit;
    osg::ref_ptr<TileService> _tileService;
    osg::ref_ptr<const Profile> _profile;
    std::string _prototype;
};


class ReaderWriterWMS : public osgDB::ReaderWriter
{
    public:
        ReaderWriterWMS() {}

        virtual const char* className()
        {
            return "WMS Reader";
        }
        
        virtual bool acceptsExtension(const std::string& extension) const
        {
            return osgDB::equalCaseInsensitive( extension, "osgearth_wms" );
        }

        virtual ReadResult readObject(const std::string& file_name, const Options* opt) const
        {
            std::string ext = osgDB::getFileExtension( file_name );
            if ( !acceptsExtension( ext ) )
            {
                return ReadResult::FILE_NOT_HANDLED;
            }

            return new WMSSource(opt);
        }
};

REGISTER_OSGPLUGIN(osgearth_wms, ReaderWriterWMS)
