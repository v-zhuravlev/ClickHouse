#include <IO/ReadHelpers.h>
#include <IO/Operators.h>

#include <Formats/verbosePrintString.h>
#include <Processors/Formats/Impl/CSVRowInputFormat.h>
#include <Formats/FormatFactory.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}


CSVRowInputFormat::CSVRowInputFormat(
    ReadBuffer & in_, Block header, Params params, bool with_names_, const FormatSettings & format_settings)
    : IRowInputFormat(std::move(header), in_, std::move(params))
    , with_names(with_names_)
    , format_settings(format_settings)
{
    auto & sample = getPort().getHeader();
    size_t num_columns = sample.columns();

    data_types.resize(num_columns);
    column_indexes_by_names.reserve(num_columns);

    for (size_t i = 0; i < num_columns; ++i)
    {
        const auto & column_info = sample.getByPosition(i);

        data_types[i] = column_info.type;
        column_indexes_by_names.emplace(column_info.name, i);
    }
}


/// Map an input file column to a table column, based on its name.
void CSVRowInputFormat::addInputColumn(const String & column_name)
{
    const auto column_it = column_indexes_by_names.find(column_name);
    if (column_it == column_indexes_by_names.end())
    {
        if (format_settings.skip_unknown_fields)
        {
            column_indexes_for_input_fields.push_back(std::nullopt);
            return;
        }

        throw Exception(
                "Unknown field found in CSV header: '" + column_name + "' " +
                "at position " + std::to_string(column_indexes_for_input_fields.size()) +
                "\nSet the 'input_format_skip_unknown_fields' parameter explicitly to ignore and proceed",
                ErrorCodes::INCORRECT_DATA
        );
    }

    const auto column_index = column_it->second;

    if (read_columns[column_index])
        throw Exception("Duplicate field found while parsing CSV header: " + column_name, ErrorCodes::INCORRECT_DATA);

    read_columns[column_index] = true;
    column_indexes_for_input_fields.emplace_back(column_index);
}

static void skipEndOfLine(ReadBuffer & istr)
{
    /// \n (Unix) or \r\n (DOS/Windows) or \n\r (Mac OS Classic)

    if (*istr.position() == '\n')
    {
        ++istr.position();
        if (!istr.eof() && *istr.position() == '\r')
            ++istr.position();
    }
    else if (*istr.position() == '\r')
    {
        ++istr.position();
        if (!istr.eof() && *istr.position() == '\n')
            ++istr.position();
        else
            throw Exception("Cannot parse CSV format: found \\r (CR) not followed by \\n (LF)."
                " Line must end by \\n (LF) or \\r\\n (CR LF) or \\n\\r.", ErrorCodes::INCORRECT_DATA);
    }
    else if (!istr.eof())
        throw Exception("Expected end of line", ErrorCodes::INCORRECT_DATA);
}


static void skipDelimiter(ReadBuffer & istr, const char delimiter, bool is_last_column)
{
    if (is_last_column)
    {
        if (istr.eof())
            return;

        /// we support the extra delimiter at the end of the line
        if (*istr.position() == delimiter)
        {
            ++istr.position();
            if (istr.eof())
                return;
        }

        skipEndOfLine(istr);
    }
    else
        assertChar(delimiter, istr);
}


/// Skip `whitespace` symbols allowed in CSV.
static inline void skipWhitespacesAndTabs(ReadBuffer & buf)
{
    while (!buf.eof()
            && (*buf.position() == ' '
                || *buf.position() == '\t'))
        ++buf.position();
}


static void skipRow(ReadBuffer & istr, const FormatSettings::CSV & settings, size_t num_columns)
{
    String tmp;
    for (size_t i = 0; i < num_columns; ++i)
    {
        skipWhitespacesAndTabs(istr);
        readCSVString(tmp, istr, settings);
        skipWhitespacesAndTabs(istr);

        skipDelimiter(istr, settings.delimiter, i + 1 == num_columns);
    }
}


void CSVRowInputFormat::readPrefix()
{
    /// In this format, we assume, that if first string field contain BOM as value, it will be written in quotes,
    ///  so BOM at beginning of stream cannot be confused with BOM in first string value, and it is safe to skip it.
    skipBOMIfExists(in);

    size_t num_columns = data_types.size();
    String tmp;

    if (with_names)
    {
        /// This CSV file has a header row with column names. Depending on the
        /// settings, use it or skip it.
        if (format_settings.with_names_use_header)
        {
            /// Look at the file header to see which columns we have there.
            /// The missing columns are filled with defaults.
            read_columns.assign(getPort().getHeader().columns(), false);
            do
            {
                String column_name;
                skipWhitespacesAndTabs(in);
                readCSVString(column_name, in, format_settings.csv);
                skipWhitespacesAndTabs(in);

                addInputColumn(column_name);
            }
            while (checkChar(format_settings.csv.delimiter, in));

            skipDelimiter(in, format_settings.csv.delimiter, true);

            for (auto read_column : read_columns)
            {
                if (read_column)
                {
                    have_always_default_columns = true;
                    break;
                }
            }

            return;
        }
        else
            skipRow(in, format_settings.csv, num_columns);
    }

    /// The default: map each column of the file to the column of the table with
    /// the same index.
    read_columns.assign(header.columns(), true);
    column_indexes_for_input_fields.resize(header.columns());

    for (size_t i = 0; i < column_indexes_for_input_fields.size(); ++i)
    {
        column_indexes_for_input_fields[i] = i;
    }
}


bool CSVRowInputFormat::readRow(MutableColumns & columns, RowReadExtension & ext)
{
    if (in.eof())
        return false;

    updateDiagnosticInfo();

    /// Track whether we have to fill any columns in this row with default
    /// values. If not, we return an empty column mask to the caller, so that
    /// it doesn't have to check it.
    bool have_default_columns = have_always_default_columns;

    const auto delimiter = format_settings.csv.delimiter;
    for (size_t file_column = 0; file_column < column_indexes_for_input_fields.size(); ++file_column)
    {
        const auto & table_column = column_indexes_for_input_fields[file_column];
        const bool is_last_file_column =
                file_column + 1 == column_indexes_for_input_fields.size();

        if (table_column)
        {
            const auto & type = data_types[*table_column];
            const bool at_delimiter = *in.position() == delimiter;
            const bool at_last_column_line_end = is_last_file_column
                                                 && (*in.position() == '\n' || *in.position() == '\r'
                                                     || in.eof());

            if (format_settings.csv.empty_as_default
                && (at_delimiter || at_last_column_line_end))
            {
                /// Treat empty unquoted column value as default value, if
                /// specified in the settings. Tuple columns might seem
                /// problematic, because they are never quoted but still contain
                /// commas, which might be also used as delimiters. However,
                /// they do not contain empty unquoted fields, so this check
                /// works for tuples as well.
                read_columns[*table_column] = false;
                have_default_columns = true;
            }
            else
            {
                /// Read the column normally.
                read_columns[*table_column] = true;
                skipWhitespacesAndTabs(in);
                type->deserializeAsTextCSV(*columns[*table_column], in,
                                           format_settings);
                skipWhitespacesAndTabs(in);
            }
        }
        else
        {
            /// We never read this column from the file, just skip it.
            String tmp;
            readCSVString(tmp, in, format_settings.csv);
        }

        skipDelimiter(in, delimiter, is_last_file_column);
    }

    if (have_default_columns)
    {
        for (size_t i = 0; i < read_columns.size(); i++)
        {
            if (!read_columns[i])
            {
                /// The column value for this row is going to be overwritten
                /// with default by the caller, but the general assumption is
                /// that the column size increases for each row, so we have
                /// to insert something. Since we do not care about the exact
                /// value, we do not have to use the default value specified by
                /// the data type, and can just use IColumn::insertDefault().
                columns[i]->insertDefault();
            }
        }
        ext.read_columns = read_columns;
    }

    return true;
}


String CSVRowInputFormat::getDiagnosticInfo()
{
    if (in.eof())        /// Buffer has gone, cannot extract information about what has been parsed.
        return {};

    WriteBufferFromOwnString out;

    auto & header = getPort().getHeader();
    MutableColumns columns = header.cloneEmptyColumns();

    /// It is possible to display detailed diagnostics only if the last and next to last rows are still in the read buffer.
    size_t bytes_read_at_start_of_buffer = in.count() - in.offset();
    if (bytes_read_at_start_of_buffer != bytes_read_at_start_of_buffer_on_prev_row)
    {
        out << "Could not print diagnostic info because two last rows aren't in buffer (rare case)\n";
        return out.str();
    }

    size_t max_length_of_column_name = 0;
    for (size_t i = 0; i < header.columns(); ++i)
        if (header.safeGetByPosition(i).name.size() > max_length_of_column_name)
            max_length_of_column_name = header.safeGetByPosition(i).name.size();

    size_t max_length_of_data_type_name = 0;
    for (size_t i = 0; i < header.columns(); ++i)
        if (header.safeGetByPosition(i).type->getName().size() > max_length_of_data_type_name)
            max_length_of_data_type_name = header.safeGetByPosition(i).type->getName().size();

    /// Roll back the cursor to the beginning of the previous or current row and parse all over again. But now we derive detailed information.

    if (pos_of_prev_row)
    {
        in.position() = pos_of_prev_row;

        out << "\nRow " << (row_num - 1) << ":\n";
        if (!parseRowAndPrintDiagnosticInfo(columns, out, max_length_of_column_name, max_length_of_data_type_name))
            return out.str();
    }
    else
    {
        if (!pos_of_current_row)
        {
            out << "Could not print diagnostic info because parsing of data hasn't started.\n";
            return out.str();
        }

        in.position() = pos_of_current_row;
    }

    out << "\nRow " << row_num << ":\n";
    parseRowAndPrintDiagnosticInfo(columns, out, max_length_of_column_name, max_length_of_data_type_name);
    out << "\n";

    return out.str();
}

/** gcc-7 generates wrong code with optimization level greater than 1.
  * See tests: dbms/src/IO/tests/write_int.cpp
  *  and dbms/tests/queries/0_stateless/00898_parsing_bad_diagnostic_message.sh
  * This is compiler bug. The bug does not present in gcc-8 and clang-8.
  * Nevertheless, we don't need high optimization of this function.
  */
bool OPTIMIZE(1) CSVRowInputFormat::parseRowAndPrintDiagnosticInfo(MutableColumns & columns,
    WriteBuffer & out, size_t max_length_of_column_name, size_t max_length_of_data_type_name)
{
    const char delimiter = format_settings.csv.delimiter;

    for (size_t file_column = 0; file_column < column_indexes_for_input_fields.size(); ++file_column)
    {
        if (file_column == 0 && in.eof())
        {
            out << "<End of stream>\n";
            return false;
        }

        if (column_indexes_for_input_fields[file_column].has_value())
        {
            const auto & table_column = *column_indexes_for_input_fields[file_column];
            const auto & current_column_type = data_types[table_column];
            const bool is_last_file_column =
                    file_column + 1 == column_indexes_for_input_fields.size();
            const bool at_delimiter = *in.position() == delimiter;
            const bool at_last_column_line_end = is_last_file_column
                                                 && (*in.position() == '\n' || *in.position() == '\r'
                                                     || in.eof());

            auto & header = getPort().getHeader();
            out << "Column " << file_column << ", " << std::string((file_column < 10 ? 2 : file_column < 100 ? 1 : 0), ' ')
                << "name: " << header.safeGetByPosition(table_column).name << ", " << std::string(max_length_of_column_name - header.safeGetByPosition(table_column).name.size(), ' ')
                << "type: " << current_column_type->getName() << ", " << std::string(max_length_of_data_type_name - current_column_type->getName().size(), ' ');

            if (format_settings.csv.empty_as_default
                && (at_delimiter || at_last_column_line_end))
            {
                columns[table_column]->insertDefault();
            }
            else
            {
                BufferBase::Position prev_position = in.position();
                BufferBase::Position curr_position = in.position();
                std::exception_ptr exception;

                try
                {
                    skipWhitespacesAndTabs(in);
                    prev_position = in.position();
                    current_column_type->deserializeAsTextCSV(*columns[table_column], in, format_settings);
                    curr_position = in.position();
                    skipWhitespacesAndTabs(in);
                }
                catch (...)
                {
                    exception = std::current_exception();
                }

                if (curr_position < prev_position)
                    throw Exception("Logical error: parsing is non-deterministic.", ErrorCodes::LOGICAL_ERROR);

                if (isNativeNumber(current_column_type) || isDateOrDateTime(current_column_type))
                {
                    /// An empty string instead of a value.
                    if (curr_position == prev_position)
                    {
                        out << "ERROR: text ";
                        verbosePrintString(prev_position, std::min(prev_position + 10, in.buffer().end()), out);
                        out << " is not like " << current_column_type->getName() << "\n";
                        return false;
                    }
                }

                out << "parsed text: ";
                verbosePrintString(prev_position, curr_position, out);

                if (exception)
                {
                    if (current_column_type->getName() == "DateTime")
                        out << "ERROR: DateTime must be in YYYY-MM-DD hh:mm:ss or NNNNNNNNNN (unix timestamp, exactly 10 digits) format.\n";
                    else if (current_column_type->getName() == "Date")
                        out << "ERROR: Date must be in YYYY-MM-DD format.\n";
                    else
                        out << "ERROR\n";
                    return false;
                }

                out << "\n";

                if (current_column_type->haveMaximumSizeOfValue()
                    && *curr_position != '\n' && *curr_position != '\r'
                    && *curr_position != delimiter)
                {
                    out << "ERROR: garbage after " << current_column_type->getName() << ": ";
                    verbosePrintString(curr_position, std::min(curr_position + 10, in.buffer().end()), out);
                    out << "\n";

                    if (current_column_type->getName() == "DateTime")
                        out << "ERROR: DateTime must be in YYYY-MM-DD hh:mm:ss or NNNNNNNNNN (unix timestamp, exactly 10 digits) format.\n";
                    else if (current_column_type->getName() == "Date")
                        out << "ERROR: Date must be in YYYY-MM-DD format.\n";

                    return false;
                }
            }
        }
        else
        {
            static const String skipped_column_str = "<SKIPPED COLUMN>";
            out << "Column " << file_column << ", " << std::string((file_column < 10 ? 2 : file_column < 100 ? 1 : 0), ' ')
                << "name: " << skipped_column_str << ", " << std::string(max_length_of_column_name - skipped_column_str.length(), ' ')
                << "type: " << skipped_column_str << ", " << std::string(max_length_of_data_type_name - skipped_column_str.length(), ' ');

            String tmp;
            readCSVString(tmp, in, format_settings.csv);
        }

        /// Delimiters
        if (file_column + 1 == column_indexes_for_input_fields.size())
        {
            if (in.eof())
                return false;

            /// we support the extra delimiter at the end of the line
            if (*in.position() == delimiter)
            {
                ++in.position();
                if (in.eof())
                    break;
            }

            if (!in.eof() && *in.position() != '\n' && *in.position() != '\r')
            {
                out << "ERROR: There is no line feed. ";
                verbosePrintString(in.position(), in.position() + 1, out);
                out << " found instead.\n"
                       " It's like your file has more columns than expected.\n"
                       "And if your file have right number of columns, maybe it have unquoted string value with comma.\n";

                return false;
            }

            skipEndOfLine(in);
        }
        else
        {
            try
            {
                assertChar(delimiter, in);
            }
            catch (const DB::Exception &)
            {
                if (*in.position() == '\n' || *in.position() == '\r')
                {
                    out << "ERROR: Line feed found where delimiter (" << delimiter << ") is expected."
                           " It's like your file has less columns than expected.\n"
                           "And if your file have right number of columns, maybe it have unescaped quotes in values.\n";
                }
                else
                {
                    out << "ERROR: There is no delimiter (" << delimiter << "). ";
                    verbosePrintString(in.position(), in.position() + 1, out);
                    out << " found instead.\n";
                }
                return false;
            }
        }
    }

    return true;
}


void CSVRowInputFormat::syncAfterError()
{
    skipToNextLineOrEOF(in);
}

void CSVRowInputFormat::updateDiagnosticInfo()
{
    ++row_num;

    bytes_read_at_start_of_buffer_on_prev_row = bytes_read_at_start_of_buffer_on_current_row;
    bytes_read_at_start_of_buffer_on_current_row = in.count() - in.offset();

    pos_of_prev_row = pos_of_current_row;
    pos_of_current_row = in.position();
}


void registerInputFormatProcessorCSV(FormatFactory & factory)
{
    for (bool with_names : {false, true})
    {
        factory.registerInputFormatProcessor(with_names ? "CSVWithNames" : "CSV", [=](
            ReadBuffer & buf,
            const Block & sample,
            const Context &,
            IRowInputFormat::Params params,
            const FormatSettings & settings)
        {
            return std::make_shared<CSVRowInputFormat>(buf, sample, std::move(params), with_names, settings);
        });
    }
}

}
