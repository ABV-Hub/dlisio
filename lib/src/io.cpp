#include <algorithm>
#include <cerrno>
#include <ciso646>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>
#include <mio/mio.hpp>
#include <lfp/lfp.h>

#include <dlisio/dlisio.h>
#include <dlisio/types.h>

#include <dlisio/ext/io.hpp>

namespace dl {

stream open(const std::string& path, std::int64_t offset) noexcept (false) {
    auto* file = std::fopen(path.c_str(), "rb");
    auto* protocol = lfp_cfile(file);
    if ( protocol == nullptr  )
        throw io_error("lfp: unable to open lfp protocol cfile");

    auto err = lfp_seek(protocol, offset);
    switch (err) {
            case LFP_OK: break;
            default:
                throw io_error(lfp_errormsg(protocol));
        }
    return stream(protocol);
}

void stream_offsets::resize( std::size_t n ) noexcept (false) {
    this->tells.resize( n );
    this->residuals.resize( n );
    this->explicits.resize( n );
}

void map_source( mio::mmap_source& file, const std::string& path ) noexcept (false) {
    std::error_code syserror;
    file.map( path, 0, mio::map_entire_file, syserror );
    if (syserror) throw std::system_error( syserror );

    if (file.size() == 0)
        throw std::invalid_argument( "non-existent or empty file" );
}

void unmap( mio::mmap_source& file ) noexcept (false) {
    file.unmap();
}

long long findsul( stream& file ) noexcept (false) {
    long long offset;

    char buffer[ 200 ];
    file.seek(0);
    auto bytes_read = file.read(buffer, 200);

    const auto err = dlis_find_sul(buffer, bytes_read, &offset);

    switch (err) {
        case DLIS_OK:
            return offset;

        case DLIS_NOTFOUND: {
            auto msg = "searched {} bytes, but could not find storage label";
            throw dl::not_found(fmt::format(msg, bytes_read));
        }

        case DLIS_INCONSISTENT: {
            auto msg = "found something that could be parts of a SUL, "
                       "file may be corrupted";
            throw std::runtime_error(msg);
        }

        default:
            throw std::runtime_error("dlis_find_sul: unknown error");
    }
}

long long findvrl( stream& file, long long from ) noexcept (false) {
    if (from < 0) {
        const auto msg = "expected from (which is {}) >= 0";
        throw std::out_of_range(fmt::format(msg, from));
    }

    long long offset;

    char buffer[ 200 ];
    file.seek(from);
    auto bytes_read = file.read(buffer, 200);
    const auto err = dlis_find_vrl(buffer, bytes_read, &offset);

    // TODO: error messages could maybe be pulled from core library
    switch (err) {
        case DLIS_OK:
            return from + offset;

        case DLIS_NOTFOUND: {
            const auto msg = "searched {} bytes, but could not find "
                             "visible record envelope pattern [0xFF 0x01]"
            ;
            throw dl::not_found(fmt::format(msg, bytes_read));
        }

        case DLIS_INCONSISTENT: {
            const auto msg = "found [0xFF 0x01] but len field not intact, "
                             "file may be corrupted";
            throw std::runtime_error(msg);
        }

        default:
            throw std::runtime_error("dlis_find_vrl: unknown error");
    }
}

stream_offsets findoffsets( mio::mmap_source& file, long long from )
noexcept (false)
{
    const auto* begin = file.data() + from;
    const auto* end = file.data() + file.size();

    constexpr std::size_t min_alloc_size = 2;
    constexpr auto resize_factor = 1.5;
    constexpr auto min_new_size = std::size_t(min_alloc_size * resize_factor);
    static_assert(min_new_size > min_alloc_size,
                                   "resize should always make size bigger");

    // by default, assume ~4K per record on average. This should be fairly few
    // reallocations, without overshooting too much
    std::size_t alloc_size = std::max(file.size() / 4096, min_alloc_size);

    stream_offsets ofs;
    ofs.resize( alloc_size );
    auto& tells     = ofs.tells;
    auto& residuals = ofs.residuals;
    auto& explicits = ofs.explicits;

    const char* next;
    int count = 0;
    int initial_residual = 0;

    while (true) {
        int err = dlis_index_records( begin,
                                      end,
                                      alloc_size,
                                      &initial_residual,
                                      &next,
                                      &count,
                                      count + tells.data(),
                                      count + residuals.data(),
                                      count + explicits.data() );

        switch (err) {
            case DLIS_OK: break;

            case DLIS_TRUNCATED:
                throw std::runtime_error( "file truncated" );

            case DLIS_INCONSISTENT:
                throw std::runtime_error( "inconsistensies in record sizes" );

            case DLIS_UNEXPECTED_VALUE: {
                // TODO: interrogate more?
                const auto msg = "record-length in record {} corrupted";
                throw std::runtime_error(fmt::format(msg, count));
            }

            default: {
                const auto msg = "dlis_index_records: unknown error {}";
                throw std::runtime_error(fmt::format(msg, err));
            }
        }

        if (next == end) break;

        const auto prev_size = tells.size();
        ofs.resize( prev_size * resize_factor );


        /* size of the now trailing newly-allocated area */
        alloc_size = tells.size() - prev_size;
        begin = next;
    }

    ofs.resize( count );

    const auto dist = file.size();
    std::transform(tells.begin(), tells.end(), tells.begin(),
            [ dist ]( long long t ) -> long long { return t += dist; } );
    return ofs;
}

bool record::isexplicit() const noexcept (true) {
    return this->attributes & DLIS_SEGATTR_EXFMTLR;
}

bool record::isencrypted() const noexcept (true) {
    return this->attributes & DLIS_SEGATTR_ENCRYPT;
}

namespace {

bool consumed_record( long long tell,
                      const std::vector< long long >& tells,
                      int i ) noexcept (true) {
    /*
     * this was the last record, so have no idea how to determine that
     * everything is properly consumed. Always true
     */
    if (std::size_t(i) == tells.size() - 1) return true;

    return tell == tells[ i + 1 ];
}

template < typename T >
bool attr_consistent( const T& ) noexcept (true) {
    // TODO: implement
    // internal attributes should have both successor and predecessor
    // first only successors, last only predecessors
    return true;
}

template < typename T >
bool type_consistent( const T& ) noexcept (true) {
    // TODO: implement
    // should be all-equal
    return true;
}

void trim_segment(std::uint8_t attrs,
                  const char* begin,
                  int segment_size,
                  std::vector< char >& segment)
noexcept (false) {
    int trim = 0;
    const auto* end = begin + segment_size;
    const auto err = dlis_trim_record_segment(attrs, begin, end, &trim);

    switch (err) {
        case DLIS_OK:
            segment.resize(segment.size() - trim);
            return;

        case DLIS_BAD_SIZE:
            if (trim - segment_size != DLIS_LRSH_SIZE) {
                const auto msg =
                    "bad segment trim: padbytes (which is {}) "
                    ">= segment.size() (which is {})";
                throw std::runtime_error(fmt::format(msg, trim, segment_size));
            }

            /*
             * padbytes included the segment header. It's larger than
             * the segment body, but only because it also counts the
             * header. accept that, pretend the body was never added,
             * and move on.
             */
            segment.resize(segment.size() - segment_size);
            return;

        default:
            throw std::invalid_argument("dlis_trim_record_segment");
    }
}

}

stream::stream( lfp_protocol* f ) noexcept (false){
    this->f = f;
}

void stream::reindex( const std::vector< long long >& tells,
                      const std::vector< int >& residuals ) noexcept (false) {
    if (tells.empty())
        throw std::invalid_argument( "tells must be non-empty" );

    if (residuals.empty())
        throw std::invalid_argument( "residuals must be non-empty" );

    if (tells.size() != residuals.size()) {
        const auto msg = "reindex requires tells.size() which is {}) "
                         "== residuals.size() (which is {})"
        ;
        const auto str = fmt::format(msg, tells.size(), residuals.size());
        throw std::invalid_argument(str);
    }

    // TODO: assert all-positive etc.
    this->tells = tells;
    this->residuals = residuals;
}

void stream::close() {
    lfp_close(this->f);
}

void stream::seek( std::int64_t offset ) noexcept (false) {
    const auto err = lfp_seek(this->f, offset);
    switch (err) {
        case LFP_OK:
            break;
        case LFP_INVALID_ARGS:
        case LFP_NOTIMPLEMENTED:
        default:
            throw std::runtime_error(lfp_errormsg( this->f ));
    }
}

std::int64_t stream::tell() const noexcept (true) {
    std::int64_t tell;
    lfp_tell(this->f, &tell);
    return tell;
}

std::int64_t stream::read( char* dst, int n )
noexcept (false) {
    std::int64_t nread = -1;
    const auto err = lfp_readinto(this->f, dst, n, &nread);
    switch (err) {
        case LFP_OK:
        case LFP_EOF:
            break;
        case LFP_OKINCOMPLETE:
        case LFP_UNEXPECTED_EOF:
        default:
            throw std::runtime_error(lfp_errormsg(this->f));
    }
    return nread;
}

record stream::at( int i ) noexcept (false) {
    record r;
    r.data.reserve( 8192 );
    return this->at( i, r );
}

/*
 * store attributes in a string to use the short-string optimisation if
 * available. Just before commit, these are checked for consistency, i.e.
 * that segments don't report inconsistent information on encryption and
 * formatting.
 */
template < typename T >
using shortvec = std::basic_string< T >;

record& stream::at( int i, record& rec ) noexcept (false) {

    auto tell = this->tells.at( i );
    auto remaining = this->residuals.at( i );

    shortvec< std::uint8_t > attributes;
    shortvec< int > types;
    bool consistent = true;

    rec.data.clear();
    this->seek(tell);

    while (true) {
        while (remaining > 0) {
            int len, type;
            std::uint8_t attrs;
            char buffer[ DLIS_LRSH_SIZE ];
            auto nread = this->read( buffer, DLIS_LRSH_SIZE );
            if ( nread < DLIS_LRSH_SIZE )
                throw std::runtime_error("stream.at: unable to read LRSH");

            const auto err = dlis_lrsh( buffer, &len, &attrs, &type );

            remaining -= len;
            len -= DLIS_LRSH_SIZE;

            if (err) consistent = false;
            attributes.push_back( attrs );
            types.push_back( type );

            if (remaining < 0) {
                /*
                 * mismatch between visisble-record-length and segment length.
                 * For now, just throw, but this could be reduced to a warning
                 * with guide on which one to believe
                 */

                const auto vrl_len = remaining + len;
                const auto cur_tell = this->tell() - DLIS_LRSH_SIZE;
                const auto msg = "visible record/segment inconsistency: "
                                 "segment (which is {}) "
                                 ">= visible (which is {}) "
                                 "in record {} (at tell {})"
                ;
                const auto str = fmt::format(msg, len, vrl_len, i, cur_tell);
                throw std::runtime_error(str);
            }

            const auto prevsize = rec.data.size();
            rec.data.resize( prevsize + len );
            nread = this->read( rec.data.data() + prevsize, len );
            if ( nread < len )
                throw std::runtime_error("stream.at: unable to read full LRS");

            /*
             * chop off trailing length and checksum for now
             * TODO: verify integrity by checking trailing length
             * TODO: calculate checksum
             */
            const auto* fst = rec.data.data() + prevsize;
            trim_segment(attrs, fst, len, rec.data);

            /*if the whole segment is getting trimmed, it's unclear if
              successor attribute should be erased or not.
              For now ignoring. Suspecting issue will never occur as
              whole "too many padbytes" problem might be caused by encryption
            */

            const auto has_successor = attrs & DLIS_SEGATTR_SUCCSEG;
            if (has_successor) continue;

            /* read last segment - check consistency and wrap up */
            if (this->contiguous and not consumed_record( this->tell(),
                                                          this->tells,
                                                          i )) {
                /*
                 * If this happens something is VERY wrong. Every new record
                 * should start just after the previous, unless bytes have been
                 * purposely skipped, because the file was otherwise broken.
                 * This probably comes from consistent, but lying, length
                 * attributes
                 */
                const auto msg = "non-contiguous record: "
                                 "#{} (at tell {}) "
                                 "ends prematurely at {}, "
                                 "not at #{} (at tell {})"
                ;

                const auto tell1 = this->tells.at(i);
                const auto tell2 = this->tells.at(i + 1);
                const auto at    = this->tell();
                const auto str   = fmt::format(msg, i, tell1, at, i+1, tell2);
                throw std::runtime_error(msg);
            }


            /*
             * The record type only cares about encryption and formatting, so only
             * extract those for checking consistency. Nothing else is interesting to
             * users, as it only describes how to read this specific segment
             */
            static const auto fmtenc = DLIS_SEGATTR_EXFMTLR | DLIS_SEGATTR_ENCRYPT;
            rec.attributes = attributes.front() & fmtenc;
            rec.type = types.front();

            rec.consistent = consistent;
            if (not attr_consistent( attributes )) rec.consistent = false;
            if (not type_consistent( types ))      rec.consistent = false;
            return rec;
        }

        int len, version;
        char buffer[ DLIS_VRL_SIZE ];
        auto nread = this->read( buffer, DLIS_VRL_SIZE );
        if ( nread < DLIS_VRL_SIZE )
            throw std::runtime_error("stream.at: Unable to read full VR");
        const auto err = dlis_vrl( buffer, &len, &version );

        // TODO: for now record closest to VE gets the blame
        if (err) consistent = false;
        if (version != 1) consistent = false;

        remaining = len - DLIS_VRL_SIZE;
    }
}

std::vector< std::pair< std::string, int > >
findfdata(mio::mmap_source& file,
          const std::vector< int >& candidates,
          const std::vector< long long >& tells,
          const std::vector< int >& residuals)
noexcept (false) {

    const auto* ptr = file.data();
    const auto* end = file.data() + file.size();
    std::vector< std::pair< std::string, int > > xs;

    char name[256] = {};

    for (auto i : candidates) {
        const auto tell = tells[i];
        const auto resi = residuals[i];
        int offset = resi == 0 ? 8 : 4;

        // read LRSH type-field
        // 0 == FDATA
        if (*(ptr + tell + offset - 1) != 0) continue;


        std::int32_t origin;
        std::uint8_t copy;
        std::int32_t idlen;
        auto cur = dlis_obname(ptr + tell + offset, &origin, &copy, &idlen, name);
        if (std::distance( cur, end ) < 0)
        {
            auto msg = "File corrupted. Error on reading fdata obname";
            throw std::runtime_error(msg);
        }

        dl::obname tmp{ dl::origin{ origin },
                        dl::ushort{ copy },
                        dl::ident{ std::string{ name, name + idlen } } };
        xs.emplace_back(tmp.fingerprint("FRAME"), i);
    }

    return xs;
}

}
