/*
	grive: an GPL program to sync a local directory with Google Drive
	Copyright (C) 2012  Wan Wai Ho

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation version 2
	of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "Drive.hh"

#include "CommonUri.hh"
#include "Entry.hh"
#include "File.hh"

#include "http/Agent.hh"
#include "http/ResponseLog.hh"
#include "http/XmlResponse.hh"
#include "protocol/Json.hh"
#include "protocol/OAuth2.hh"
#include "util/Destroy.hh"
#include "util/Log.hh"
//#include "util/Path.hh"
#include "xml/Node.hh"
#include "xml/NodeSet.hh"

#include <boost/bind.hpp>

// standard C++ library
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

// for debugging only
#include <iostream>

namespace gr {

namespace
{
	const std::string state_file = ".grive_state" ;
}

Drive::Drive( OAuth2& auth ) :
	m_auth( auth ),
	m_state( state_file )
{
	m_http_hdr.push_back( "Authorization: Bearer " + m_auth.AccessToken() ) ;
	m_http_hdr.push_back( "GData-Version: 3.0" ) ;

	http::Agent http ;
	http::XmlResponse xrsp ;
	http.Get( feed_metadata, &xrsp, m_http_hdr ) ;
	
	std::string change_stamp = xrsp.Response()["docs:largestChangestamp"]["@value"] ;
	Trace( "change stamp is %1%", change_stamp ) ;

	m_state.ChangeStamp( change_stamp ) ;
	m_state.Sync( "." ) ;
	
	ConstructDirTree( &http ) ;
	return ;
	
	std::string uri = feed_base + "?showfolders=true&showroot=true" ;
/*	if ( !change_stamp.empty() )
	{
		int ichangestamp = std::atoi( change_stamp.c_str() ) + 1 ;
		uri = (boost::format( "%1%&start-index=%2%" ) % uri % ichangestamp ).str() ;
	}
*/
	http.Get( uri, &xrsp, m_http_hdr ) ;
	xml::Node resp = xrsp.Response() ;

	m_resume_link = resp["link"].
		Find( "@rel", "http://schemas.google.com/g/2005#resumable-create-media" )["@href"] ;

	bool has_next = false ;
	do
	{
		xml::NodeSet entries = resp["entry"] ;
		for ( xml::NodeSet::iterator i = entries.begin() ; i != entries.end() ; ++i )
		{
			if ( (*i)["content"] == "" )
				continue ;
		
			Entry file( *i ) ;
			if ( file.Kind() != "folder" )
			{
				FolderListIterator pit = FindFolder( file.ParentHref() ) ;
				if ( pit != m_coll.end() && pit->IsInRootTree() )
					UpdateFile( file, *pit, &http ) ;
				else
					Log( "file \"%1%\" parent doesn't exist, ignored", file.Title() ) ;
			}
		}
		
		xml::NodeSet nss = resp["link"].Find( "@rel", "next" ) ;
		has_next = !nss.empty() ;

		if ( has_next )
		{
			http::ResponseLog log2( "second-", ".xml", &xrsp ) ;
			http.Get( nss["@href"], &log2, m_http_hdr ) ;
			resp = xrsp.Response() ;
		}
	} while ( has_next ) ;
	
	m_state.Write( state_file ) ;
}

Drive::~Drive( )
{
	std::for_each( m_files.begin(), m_files.end(), Destroy() ) ;
}

struct SortCollectionByHref
{
	bool operator()( const Collection& c1, const Collection& c2 ) const
	{
		return c1.SelfHref() < c2.SelfHref() ;
	}
} ;

Drive::FolderListIterator Drive::FindFolder( const std::string& href )
{
	// try to find the parent by its href
	std::pair<FolderListIterator,FolderListIterator> its =
		std::equal_range(
			m_coll.begin(),
			m_coll.end(),
			Collection( "", href ),
			SortCollectionByHref() ) ;
	
	 return (its.first != its.second) ? its.first : m_coll.end() ;
}

Drive::FolderListIterator Drive::Root( )
{
	FolderListIterator root = FindFolder( root_href ) ;
	assert( root != m_coll.end() ) ;
	return root ;
}

void Drive::ConstructDirTree( http::Agent *http )
{
	http::XmlResponse xml ;
	http::ResponseLog log( "dir-", ".xml", &xml ) ;
	
	http->Get( feed_base + "/-/folder?max-results=10&showroot=true", &log, m_http_hdr ) ;

	xml::Node resp = xml.Response() ;

	assert( m_coll.empty() ) ;
	m_coll.push_back( Collection( ".", root_href ) ) ;
	
	while ( true )
	{
		xml::NodeSet entries = resp["entry"] ;

		// first, get all collections from the query result
		for ( xml::NodeSet::iterator i = entries.begin() ; i != entries.end() ; ++i )
		{
			Entry e( *i ) ;
			if ( e.Kind() == "folder" )
			{
				if ( e.ParentHrefs().size() == 1 )
				{
					m_state.OnEntry( e ) ;
					m_coll.push_back( Collection( e ) ) ;
				}
				else
					Log( "folder \"%1%\" has multiple parents, ignored", e.Title(), log::warning ) ;
			}
		}
		
		xml::NodeSet next = resp["link"].Find( "@rel", "next" ) ;
		if ( next.empty() )
			break ;

		http->Get( next["@href"], &xml, m_http_hdr ) ;
		resp = xml.Response() ;
	}

	m_state.ResolveEntry() ;
	
	// second, build up linkage between parent and child 
	std::sort( m_coll.begin(), m_coll.end(), SortCollectionByHref() ) ;
	for ( FolderListIterator i = m_coll.begin() ; i != m_coll.end() ; ++i )
	{
		FolderListIterator pit = FindFolder( i->ParentHref() ) ;
		Collection *scoll = m_state.FindFolderByHref( i->SelfHref() ) ;
		if ( scoll )
			Trace( "found folder %1% in state", scoll->Title() ) ;
		else
			Trace( "can't found folder %1% in state", i->Title() ) ;
		
		if ( pit != m_coll.end() )
		{
			// it shouldn't happen, just in case
			if ( &*i == &*pit )
				Log( "the parent of folder %1% is itself, ignored.", i->Title(), log::warning ) ;
			else
				pit->AddChild( &*i ) ;
		}
		else
			Log( "can't find folder \"%1%\" (\"%2%\")", i->Title(), i->ParentHref(), log::warning ) ;
	}

	// lastly, iterating from the root, create the directories in the local file system
	assert( Root()->Parent() == 0 ) ;
	Root()->CreateSubDir( "." ) ;
}

void Drive::UpdateFile( Entry& entry, Collection& parent, http::Agent *http )
{
	// only handle uploaded files
	if ( !entry.Filename().empty() )
	{
		File *file = new File( entry, &parent ) ;
		m_files.push_back( file ) ;
		parent.AddLeaf( file ) ;
		
// 		Trace( "%1% ID = %2%", file->Path(), file->ResourceID() ) ;
		
// 		m_state.SetId( file->Path(), file->ResourceID() ) ;
		
// 		file->Update( http, m_http_hdr ) ;
	}
	else
	{
		Log( "file \"%1%\" is a google document, ignored", entry.Title() ) ;
	}
}

void Drive::Update()
{
	http::Agent http ;
	std::for_each( m_files.begin(), m_files.end(),
		boost::bind( &File::Update, _1, &http, m_http_hdr ) ) ;
}

} // end of namespace
