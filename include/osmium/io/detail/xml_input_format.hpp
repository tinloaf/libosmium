#ifndef OSMIUM_IO_DETAIL_XML_INPUT_FORMAT_HPP
#define OSMIUM_IO_DETAIL_XML_INPUT_FORMAT_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2016 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <expat.h>

#include <osmium/builder/builder.hpp>
#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/detail/input_format.hpp>
#include <osmium/io/detail/queue_util.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/header.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/osm/types_from_string.hpp>
#include <osmium/thread/queue.hpp>
#include <osmium/thread/util.hpp>
#include <osmium/util/cast.hpp>

namespace osmium {

    /**
     * Exception thrown when the XML parser failed. The exception contains
     * (if available) information about the place where the error happened
     * and the type of error.
     */
    struct xml_error : public io_error {

        unsigned long line;
        unsigned long column;
        XML_Error error_code;
        std::string error_string;

        explicit xml_error(XML_Parser parser) :
            io_error(std::string("XML parsing error at line ")
                    + std::to_string(XML_GetCurrentLineNumber(parser))
                    + ", column "
                    + std::to_string(XML_GetCurrentColumnNumber(parser))
                    + ": "
                    + XML_ErrorString(XML_GetErrorCode(parser))),
            line(XML_GetCurrentLineNumber(parser)),
            column(XML_GetCurrentColumnNumber(parser)),
            error_code(XML_GetErrorCode(parser)),
            error_string(XML_ErrorString(error_code)) {
        }

        explicit xml_error(const std::string& message) :
            io_error(message),
            line(0),
            column(0),
            error_code(),
            error_string(message) {
        }

    }; // struct xml_error

    /**
     * Exception thrown when an OSM XML files contains no version attribute
     * on the 'osm' element or if the version is unknown.
     */
    struct format_version_error : public io_error {

        std::string version;

        explicit format_version_error() :
            io_error("Can not read file without version (missing version attribute on osm element)."),
            version() {
        }

        explicit format_version_error(const char* v) :
            io_error(std::string("Can not read file with version ") + v),
            version(v) {
        }

    }; // struct format_version_error

    namespace io {

        namespace detail {

            /**
             * Helper function to avoid using std::atof, since for std::atof,
             * the decimal separator may not be "." (depending on the locale),
             * leading to coordinates being cut down to their integer parts.
             */
            inline double atof_helper(const char *str) {
              double val = 0.0f;
              std::istringstream istr(str);
              istr.imbue(std::locale("C"));
              istr >> val;
              return val;
            }

            class XMLParser : public Parser {

                static constexpr int buffer_size = 2 * 1000 * 1000;

                enum class context {
                    root,
                    top,
                    node,
                    way,
                    relation,
                    changeset,
                    discussion,
                    comment,
                    comment_text,
                    ignored_node,
                    ignored_way,
                    ignored_relation,
                    ignored_changeset,
                    in_object
                }; // enum class context

                context m_context;
                context m_last_context;

                /**
                 * This is used only for change files which contain create, modify,
                 * and delete sections.
                 */
                bool m_in_delete_section;

                osmium::io::Header m_header;

                osmium::memory::Buffer m_buffer;

                std::unique_ptr<osmium::builder::NodeBuilder>                m_node_builder;
                std::unique_ptr<osmium::builder::WayBuilder>                 m_way_builder;
                std::unique_ptr<osmium::builder::RelationBuilder>            m_relation_builder;
                std::unique_ptr<osmium::builder::ChangesetBuilder>           m_changeset_builder;
                std::unique_ptr<osmium::builder::ChangesetDiscussionBuilder> m_changeset_discussion_builder;

                std::unique_ptr<osmium::builder::TagListBuilder>             m_tl_builder;
                std::unique_ptr<osmium::builder::WayNodeListBuilder>         m_wnl_builder;
                std::unique_ptr<osmium::builder::RelationMemberListBuilder>  m_rml_builder;

                std::string m_comment_text;

                /**
                 * A C++ wrapper for the Expat parser that makes sure no memory is leaked.
                 */
                template <typename T>
                class ExpatXMLParser {

                    XML_Parser m_parser;

                    static void XMLCALL start_element_wrapper(void* data, const XML_Char* element, const XML_Char** attrs) {
                        static_cast<XMLParser*>(data)->start_element(element, attrs);
                    }

                    static void XMLCALL end_element_wrapper(void* data, const XML_Char* element) {
                        static_cast<XMLParser*>(data)->end_element(element);
                    }

                    static void XMLCALL character_data_wrapper(void* data, const XML_Char* text, int len) {
                        static_cast<XMLParser*>(data)->characters(text, len);
                    }

                    // This handler is called when there are any XML entities
                    // declared in the OSM file. Entities are normally not used,
                    // but they can be misused. See
                    // https://en.wikipedia.org/wiki/Billion_laughs
                    // The handler will just throw an error.
                    static void entity_declaration_handler(void*,
                            const XML_Char*, int, const XML_Char*, int, const XML_Char*,
                            const XML_Char*, const XML_Char*, const XML_Char*) {
                        throw osmium::xml_error("XML entities are not supported");
                    }

                public:

                    explicit ExpatXMLParser(T* callback_object) :
                        m_parser(XML_ParserCreate(nullptr)) {
                        if (!m_parser) {
                            throw osmium::io_error("Internal error: Can not create parser");
                        }
                        XML_SetUserData(m_parser, callback_object);
                        XML_SetElementHandler(m_parser, start_element_wrapper, end_element_wrapper);
                        XML_SetCharacterDataHandler(m_parser, character_data_wrapper);
                        XML_SetEntityDeclHandler(m_parser, entity_declaration_handler);
                    }

                    ExpatXMLParser(const ExpatXMLParser&) = delete;
                    ExpatXMLParser(ExpatXMLParser&&) = delete;

                    ExpatXMLParser& operator=(const ExpatXMLParser&) = delete;
                    ExpatXMLParser& operator=(ExpatXMLParser&&) = delete;

                    ~ExpatXMLParser() noexcept {
                        XML_ParserFree(m_parser);
                    }

                    void operator()(const std::string& data, bool last) {
                        if (XML_Parse(m_parser, data.data(), static_cast_with_assert<int>(data.size()), last) == XML_STATUS_ERROR) {
                            throw osmium::xml_error(m_parser);
                        }
                    }

                }; // class ExpatXMLParser

                template <typename T>
                static void check_attributes(const XML_Char** attrs, T check) {
                    while (*attrs) {
                        check(attrs[0], attrs[1]);
                        attrs += 2;
                    }
                }

                const char* init_object(osmium::OSMObject& object, const XML_Char** attrs) {
                    const char* user = "";

                    if (m_in_delete_section) {
                        object.set_visible(false);
                    }

                    osmium::Location location;

                    check_attributes(attrs, [&location, &user, &object](const XML_Char* name, const XML_Char* value) {
                        if (!std::strcmp(name, "lon")) {
                            location.set_lon(atof_helper(value)); // XXX doesn't detect garbage after the number
                        } else if (!std::strcmp(name, "lat")) {
                            location.set_lat(atof_helper(value)); // XXX doesn't detect garbage after the number
                        } else if (!std::strcmp(name, "user")) {
                            user = value;
                        } else {
                            object.set_attribute(name, value);
                        }
                    });

                    if (location && object.type() == osmium::item_type::node) {
                        static_cast<osmium::Node&>(object).set_location(location);
                    }

                    return user;
                }

                void init_changeset(osmium::builder::ChangesetBuilder* builder, const XML_Char** attrs) {
                    const char* user = "";
                    osmium::Changeset& new_changeset = builder->object();

                    osmium::Location min;
                    osmium::Location max;
                    check_attributes(attrs, [&min, &max, &user, &new_changeset](const XML_Char* name, const XML_Char* value) {
                        if (!std::strcmp(name, "min_lon")) {
                            min.set_lon(atof_helper(value));
                        } else if (!std::strcmp(name, "min_lat")) {
                            min.set_lat(atof_helper(value));
                        } else if (!std::strcmp(name, "max_lon")) {
                            max.set_lon(atof_helper(value));
                        } else if (!std::strcmp(name, "max_lat")) {
                            max.set_lat(atof_helper(value));
                        } else if (!std::strcmp(name, "user")) {
                            user = value;
                        } else {
                            new_changeset.set_attribute(name, value);
                        }
                    });

                    new_changeset.bounds().extend(min);
                    new_changeset.bounds().extend(max);

                    builder->add_user(user);
                }

                void get_tag(osmium::builder::Builder* builder, const XML_Char** attrs) {
                    const char* k = "";
                    const char* v = "";
                    check_attributes(attrs, [&k, &v](const XML_Char* name, const XML_Char* value) {
                        if (name[0] == 'k' && name[1] == 0) {
                            k = value;
                        } else if (name[0] == 'v' && name[1] == 0) {
                            v = value;
                        }
                    });
                    if (!m_tl_builder) {
                        m_tl_builder = std::unique_ptr<osmium::builder::TagListBuilder>(new osmium::builder::TagListBuilder(m_buffer, builder));
                    }
                    m_tl_builder->add_tag(k, v);
                }

                void mark_header_as_done() {
                    set_header_value(m_header);
                }

                void start_element(const XML_Char* element, const XML_Char** attrs) {
                    switch (m_context) {
                        case context::root:
                            if (!std::strcmp(element, "osm") || !std::strcmp(element, "osmChange")) {
                                if (!std::strcmp(element, "osmChange")) {
                                    m_header.set_has_multiple_object_versions(true);
                                }
                                check_attributes(attrs, [this](const XML_Char* name, const XML_Char* value) {
                                    if (!std::strcmp(name, "version")) {
                                        m_header.set("version", value);
                                        if (std::strcmp(value, "0.6")) {
                                            throw osmium::format_version_error(value);
                                        }
                                    } else if (!std::strcmp(name, "generator")) {
                                        m_header.set("generator", value);
                                    }
                                });
                                if (m_header.get("version") == "") {
                                    throw osmium::format_version_error();
                                }
                            } else {
                                throw osmium::xml_error(std::string("Unknown top-level element: ") + element);
                            }
                            m_context = context::top;
                            break;
                        case context::top:
                            assert(!m_tl_builder);
                            if (!std::strcmp(element, "node")) {
                                mark_header_as_done();
                                if (read_types() & osmium::osm_entity_bits::node) {
                                    m_node_builder = std::unique_ptr<osmium::builder::NodeBuilder>(new osmium::builder::NodeBuilder(m_buffer));
                                    m_node_builder->add_user(init_object(m_node_builder->object(), attrs));
                                    m_context = context::node;
                                } else {
                                    m_context = context::ignored_node;
                                }
                            } else if (!std::strcmp(element, "way")) {
                                mark_header_as_done();
                                if (read_types() & osmium::osm_entity_bits::way) {
                                    m_way_builder = std::unique_ptr<osmium::builder::WayBuilder>(new osmium::builder::WayBuilder(m_buffer));
                                    m_way_builder->add_user(init_object(m_way_builder->object(), attrs));
                                    m_context = context::way;
                                } else {
                                    m_context = context::ignored_way;
                                }
                            } else if (!std::strcmp(element, "relation")) {
                                mark_header_as_done();
                                if (read_types() & osmium::osm_entity_bits::relation) {
                                    m_relation_builder = std::unique_ptr<osmium::builder::RelationBuilder>(new osmium::builder::RelationBuilder(m_buffer));
                                    m_relation_builder->add_user(init_object(m_relation_builder->object(), attrs));
                                    m_context = context::relation;
                                } else {
                                    m_context = context::ignored_relation;
                                }
                            } else if (!std::strcmp(element, "changeset")) {
                                mark_header_as_done();
                                if (read_types() & osmium::osm_entity_bits::changeset) {
                                    m_changeset_builder = std::unique_ptr<osmium::builder::ChangesetBuilder>(new osmium::builder::ChangesetBuilder(m_buffer));
                                    init_changeset(m_changeset_builder.get(), attrs);
                                    m_context = context::changeset;
                                } else {
                                    m_context = context::ignored_changeset;
                                }
                            } else if (!std::strcmp(element, "bounds")) {
                                osmium::Location min;
                                osmium::Location max;
                                check_attributes(attrs, [&min, &max](const XML_Char* name, const XML_Char* value) {
                                    if (!std::strcmp(name, "minlon")) {
                                        min.set_lon(atof_helper(value));
                                    } else if (!std::strcmp(name, "minlat")) {
                                        min.set_lat(atof_helper(value));
                                    } else if (!std::strcmp(name, "maxlon")) {
                                        max.set_lon(atof_helper(value));
                                    } else if (!std::strcmp(name, "maxlat")) {
                                        max.set_lat(atof_helper(value));
                                    }
                                });
                                osmium::Box box;
                                box.extend(min).extend(max);
                                m_header.add_box(box);
                            } else if (!std::strcmp(element, "delete")) {
                                m_in_delete_section = true;
                            }
                            break;
                        case context::node:
                            m_last_context = context::node;
                            m_context = context::in_object;
                            if (!std::strcmp(element, "tag")) {
                                get_tag(m_node_builder.get(), attrs);
                            }
                            break;
                        case context::way:
                            m_last_context = context::way;
                            m_context = context::in_object;
                            if (!std::strcmp(element, "nd")) {
                                m_tl_builder.reset();

                                if (!m_wnl_builder) {
                                    m_wnl_builder = std::unique_ptr<osmium::builder::WayNodeListBuilder>(new osmium::builder::WayNodeListBuilder(m_buffer, m_way_builder.get()));
                                }

                                NodeRef nr;
                                check_attributes(attrs, [this, &nr](const XML_Char* name, const XML_Char* value) {
                                    if (!std::strcmp(name, "ref")) {
                                        nr.set_ref(osmium::string_to_object_id(value));
                                    } else if (!std::strcmp(name, "lon")) {
                                        nr.location().set_lon(atof_helper(value)); // XXX doesn't detect garbage after the number
                                    } else if (!std::strcmp(name, "lat")) {
                                        nr.location().set_lat(atof_helper(value)); // XXX doesn't detect garbage after the number
                                    }
                                });
                                m_wnl_builder->add_node_ref(nr);
                            } else if (!std::strcmp(element, "tag")) {
                                m_wnl_builder.reset();
                                get_tag(m_way_builder.get(), attrs);
                            }
                            break;
                        case context::relation:
                            m_last_context = context::relation;
                            m_context = context::in_object;
                            if (!std::strcmp(element, "member")) {
                                m_tl_builder.reset();

                                if (!m_rml_builder) {
                                    m_rml_builder = std::unique_ptr<osmium::builder::RelationMemberListBuilder>(new osmium::builder::RelationMemberListBuilder(m_buffer, m_relation_builder.get()));
                                }

                                item_type type = item_type::undefined;
                                object_id_type ref = 0;
                                const char* role = "";
                                check_attributes(attrs, [&type, &ref, &role](const XML_Char* name, const XML_Char* value) {
                                    if (!std::strcmp(name, "type")) {
                                        type = char_to_item_type(value[0]);
                                    } else if (!std::strcmp(name, "ref")) {
                                        ref = osmium::string_to_object_id(value);
                                    } else if (!std::strcmp(name, "role")) {
                                        role = static_cast<const char*>(value);
                                    }
                                });
                                if (type != item_type::node && type != item_type::way && type != item_type::relation) {
                                    throw osmium::xml_error("Unknown type on relation member");
                                }
                                if (ref == 0) {
                                    throw osmium::xml_error("Missing ref on relation member");
                                }
                                m_rml_builder->add_member(type, ref, role);
                            } else if (!std::strcmp(element, "tag")) {
                                m_rml_builder.reset();
                                get_tag(m_relation_builder.get(), attrs);
                            }
                            break;
                        case context::changeset:
                            m_last_context = context::changeset;
                            if (!std::strcmp(element, "discussion")) {
                                m_context = context::discussion;
                                m_tl_builder.reset();
                                if (!m_changeset_discussion_builder) {
                                    m_changeset_discussion_builder = std::unique_ptr<osmium::builder::ChangesetDiscussionBuilder>(new osmium::builder::ChangesetDiscussionBuilder(m_buffer, m_changeset_builder.get()));
                                }
                            } else if (!std::strcmp(element, "tag")) {
                                m_context = context::in_object;
                                m_changeset_discussion_builder.reset();
                                get_tag(m_changeset_builder.get(), attrs);
                            }
                            break;
                        case context::discussion:
                            if (!std::strcmp(element, "comment")) {
                                m_context = context::comment;
                                osmium::Timestamp date;
                                osmium::user_id_type uid = 0;
                                const char* user = "";
                                check_attributes(attrs, [&date, &uid, &user](const XML_Char* name, const XML_Char* value) {
                                    if (!std::strcmp(name, "date")) {
                                        date = osmium::Timestamp(value);
                                    } else if (!std::strcmp(name, "uid")) {
                                        uid = osmium::string_to_user_id(value);
                                    } else if (!std::strcmp(name, "user")) {
                                        user = static_cast<const char*>(value);
                                    }
                                });
                                m_changeset_discussion_builder->add_comment(date, uid, user);
                            }
                            break;
                        case context::comment:
                            if (!std::strcmp(element, "text")) {
                                m_context = context::comment_text;
                            }
                            break;
                        case context::comment_text:
                            break;
                        case context::ignored_node:
                            break;
                        case context::ignored_way:
                            break;
                        case context::ignored_relation:
                            break;
                        case context::ignored_changeset:
                            break;
                        case context::in_object:
                            assert(false); // should never be here
                            break;
                    }
                }

                void end_element(const XML_Char* element) {
                    switch (m_context) {
                        case context::root:
                            assert(false); // should never be here
                            break;
                        case context::top:
                            if (!std::strcmp(element, "osm") || !std::strcmp(element, "osmChange")) {
                                mark_header_as_done();
                                m_context = context::root;
                            } else if (!std::strcmp(element, "delete")) {
                                m_in_delete_section = false;
                            }
                            break;
                        case context::node:
                            assert(!std::strcmp(element, "node"));
                            m_tl_builder.reset();
                            m_node_builder.reset();
                            m_buffer.commit();
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::way:
                            assert(!std::strcmp(element, "way"));
                            m_tl_builder.reset();
                            m_wnl_builder.reset();
                            m_way_builder.reset();
                            m_buffer.commit();
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::relation:
                            assert(!std::strcmp(element, "relation"));
                            m_tl_builder.reset();
                            m_rml_builder.reset();
                            m_relation_builder.reset();
                            m_buffer.commit();
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::changeset:
                            assert(!std::strcmp(element, "changeset"));
                            m_tl_builder.reset();
                            m_changeset_discussion_builder.reset();
                            m_changeset_builder.reset();
                            m_buffer.commit();
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::discussion:
                            assert(!std::strcmp(element, "discussion"));
                            m_context = context::changeset;
                            break;
                        case context::comment:
                            assert(!std::strcmp(element, "comment"));
                            m_context = context::discussion;
                            break;
                        case context::comment_text:
                            assert(!std::strcmp(element, "text"));
                            m_context = context::comment;
                            m_changeset_discussion_builder->add_comment_text(m_comment_text);
                            break;
                        case context::in_object:
                            m_context = m_last_context;
                            break;
                        case context::ignored_node:
                            if (!std::strcmp(element, "node")) {
                                m_context = context::top;
                            }
                            break;
                        case context::ignored_way:
                            if (!std::strcmp(element, "way")) {
                                m_context = context::top;
                            }
                            break;
                        case context::ignored_relation:
                            if (!std::strcmp(element, "relation")) {
                                m_context = context::top;
                            }
                            break;
                        case context::ignored_changeset:
                            if (!std::strcmp(element, "changeset")) {
                                m_context = context::top;
                            }
                            break;
                    }
                }

                void characters(const XML_Char* text, int len) {
                    if (m_context == context::comment_text) {
                        m_comment_text.append(text, len);
                    } else {
                        m_comment_text.resize(0);
                    }
                }

                void flush_buffer() {
                    if (m_buffer.committed() > buffer_size / 10 * 9) {
                        send_to_output_queue(std::move(m_buffer));
                        osmium::memory::Buffer buffer(buffer_size);
                        using std::swap;
                        swap(m_buffer, buffer);
                    }
                }

            public:

                XMLParser(future_string_queue_type& input_queue,
                          future_buffer_queue_type& output_queue,
                          std::promise<osmium::io::Header>& header_promise,
                          osmium::osm_entity_bits::type read_types) :
                    Parser(input_queue, output_queue, header_promise, read_types),
                    m_context(context::root),
                    m_last_context(context::root),
                    m_in_delete_section(false),
                    m_header(),
                    m_buffer(buffer_size),
                    m_node_builder(),
                    m_way_builder(),
                    m_relation_builder(),
                    m_changeset_builder(),
                    m_changeset_discussion_builder(),
                    m_tl_builder(),
                    m_wnl_builder(),
                    m_rml_builder() {
                }

                ~XMLParser() noexcept final = default;

                void run() final {
                    osmium::thread::set_thread_name("_osmium_xml_in");

                    ExpatXMLParser<XMLParser> parser(this);

                    while (!input_done()) {
                        std::string data = get_input();
                        parser(data, input_done());
                        if (read_types() == osmium::osm_entity_bits::nothing && header_is_done()) {
                            break;
                        }
                    }

                    mark_header_as_done();

                    if (m_buffer.committed() > 0) {
                        send_to_output_queue(std::move(m_buffer));
                    }
                }

            }; // class XMLParser

            // we want the register_parser() function to run, setting
            // the variable is only a side-effect, it will never be used
            const bool registered_xml_parser = ParserFactory::instance().register_parser(
                file_format::xml,
                [](future_string_queue_type& input_queue,
                    future_buffer_queue_type& output_queue,
                    std::promise<osmium::io::Header>& header_promise,
                    osmium::osm_entity_bits::type read_which_entities) {
                    return std::unique_ptr<Parser>(new XMLParser(input_queue, output_queue, header_promise, read_which_entities));
            });

            // dummy function to silence the unused variable warning from above
            inline bool get_registered_xml_parser() noexcept {
                return registered_xml_parser;
            }

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_XML_INPUT_FORMAT_HPP
