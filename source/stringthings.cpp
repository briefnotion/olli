// ***************************************************************************************
// *
// *    Core       | Everything within this document is proprietary to Core Dynamics.
// *    Dynamics   | Any unauthorized duplication will be subject to prosecution.
// *
// *    Department : (R+D)^2                        Name: stringthings.cpp
// *       Sub Dept: Programming
// *    Location ID: 856-45B
// *                                                      (c) 2856 - 2858 Core Dynamics
// ***************************************************************************************

#ifndef STRINGTHINGS_CPP
#define STRINGTHINGS_CPP

#include "stringthings.h"

using namespace std;

int count_char_in_string(string& Text, char Character)
{
  return static_cast<int>(std::count(Text.begin(), Text.end(), Character));
}

string char_buf_to_string(char Buf[], int Buf_Len)
{
  return std::string(Buf, static_cast<std::size_t>(Buf_Len));

  //char response[64];
  //string thing = char_buf_to_string(response, 64);
  //printf ("thing: %c %s \n", response[0], thing.c_str());

  //return ret_str;
}

/**
 * @brief Filters out unwanted LLM artifacts like markdown formatting for clean TTS output.
 */
std::string tts_filter(const std::string& text) {
    std::string result = text;
    
    // 1. Remove Markdown Bold/Italic indicators (** , __ , * , _)
    const std::vector<std::string> md_artifacts = {"**", "__", "*", "_", "`", "#"};
    for (const auto& artifact : md_artifacts) {
        size_t pos = 0;
        while ((pos = result.find(artifact, pos)) != std::string::npos) {
            result.erase(pos, artifact.length());
        }
    }

    // 2. Clean up excessive whitespace/newlines that cause awkward TTS pauses
    std::string cleaned;
    bool last_was_space = false;
    for (size_t i = 0; i < result.length(); ++i) {
        if (std::isspace(static_cast<unsigned char>(result[i]))) {
            if (!last_was_space) {
                cleaned += ' ';
                last_was_space = true;
            }
        } else {
            cleaned += result[i];
            last_was_space = false;
        }
    }

    return cleaned;
}

string filter_non_printable(const std::string& input)
{
  string result = "";

  for (char c : input) 
  {
    if (isprint(static_cast<unsigned char>(c)) || c == '\n')
    {
      result += c;
    }
    else
    {
      result += '.';
    }
  }

  return result;
}

string filter_all_non_printable(const std::string& input)
{
  string result = "";

  for (char c : input) 
  {
    if (isprint(static_cast<unsigned char>(c)))
    {
      result += c;
    }
  }

  return result;
}

string line_create(int Size, char Character)
{
  string line = "";

  return line.append(static_cast<std::size_t>(Size), Character);
}

bool starts_with(const std::string& s, const std::string& prefix) 
{
    if (prefix.size() > s.size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i) 
    {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) 
        {
            return false;
        }
    }
    return true;
}

std::string linefill(int size, std::string text)
// Returns a space-filled line of size with text in center.
{
    // 1. Handle edge case: if size is negative or zero
    if (size <= 0) return "";

    // 2. Create the base string of spaces (cast size to size_t)
    std::string line(static_cast<std::size_t>(size), ' ');

    // 3. Calculate centering math using signed integers to avoid wrap-around bugs
    int text_len = static_cast<int>(text.length());
    int start_pos = (size / 2) - (text_len / 2);

    // 4. Only replace if the text actually fits or starts within bounds
    if (start_pos < 0) start_pos = 0;
    
    // Ensure we don't try to replace more than the line can hold
    std::size_t safe_len = std::min(static_cast<std::size_t>(text_len), 
                                    static_cast<std::size_t>(size - start_pos));

    line.replace(static_cast<std::size_t>(start_pos), safe_len, text.substr(0, safe_len));

    return line;
}

std::string linemerge_left_justify(int size, std::string line, std::string text)
// Overlaps and left justifies text onto line.
// Returns value at size of size. 
{
    std::string return_string = line;

    // 1. Overlay text onto the start of the string
    if (return_string.size() > text.size())
    {
        return_string.replace(0, text.size(), text);
    }
    else
    {
        return_string = text;
    }

    // 2. Ensure the string matches the requested 'size'
    // Use static_cast for clear, safe conversion to unsigned size_t
    std::size_t target_size = (size > 0) ? static_cast<std::size_t>(size) : 0;

    if (return_string.size() > target_size)
    {
        // Trim if too long
        return_string.erase(target_size);
    }
    else if (return_string.size() < target_size)
    {
        // Pad with spaces if too short (to ensure it actually returns 'size')
        return_string.append(target_size - return_string.size(), ' ');
    }

    return return_string;
}

string linemerge_left_justify(string line, string text)
// Overlaps and left justifies text onto line.
// No Truncate.
// Returns value at size of line or text size, whichever
//  is larger. 
{
  return linemerge_left_justify(0, line, text);
}

std::string linemerge_right_justify(int size, std::string line, std::string text)
// Overlaps and right justifies text onto line.
// Returns value at size. 
{
    std::string return_string = line;
    std::size_t text_sz = text.size();
    std::size_t line_sz = return_string.size();

    // 1. Overlay text onto the end of the string
    if (line_sz > text_sz)
    {
        // Safe subtraction because we checked line_sz > text_sz
        return_string.replace(line_sz - text_sz, text_sz, text);
    }
    else
    {
        // Text is larger or equal, so it effectively becomes the string
        return_string = text;
    }

    // 2. Adjust to requested 'size'
    std::size_t target_size = (size > 0) ? static_cast<std::size_t>(size) : 0;

    if (return_string.size() > target_size)
    {
        // Trim from the LEFT to keep the right-justified text
        // Use the difference for the number of characters to delete
        return_string.erase(0, return_string.size() - target_size);
    }
    else if (return_string.size() < target_size)
    {
        // If shorter than target size, pad the LEFT with spaces
        return_string.insert(0, target_size - return_string.size(), ' ');
    }

    return return_string;
}

std::string right_justify(int size, std::string text)
// Returns a string of 'size' with 'text' aligned to the right.
{
    std::size_t target_size = (size > 0) ? static_cast<std::size_t>(size) : 0;
    
    // Case 1: Text is too long - Trim from the left
    if (text.size() > target_size)
    {
        // We use substr instead of erase to avoid modifying the input 'text' 
        // and to keep the logic cleaner.
        return text.substr(text.size() - target_size);
    }
    
    // Case 2: Text is exactly the right size
    if (text.size() == target_size)
    {
        return text;
    }

    // Case 3: Text is shorter than size - Pad with leading spaces
    // No need for 'return_string = ""', just build it directly.
    return std::string(target_size - text.size(), ' ') + text;
}

std::string left_justify(int size, std::string text)
// Overlaps and left justifies text onto line.
// Returns value at size. 
{
    // Ensure size isn't negative before converting to unsigned
    std::size_t target_size = (size > 0) ? static_cast<std::size_t>(size) : 0;

    // resize() handles both cases:
    // 1. If text is too long, it truncates the right side.
    // 2. If text is too short, it pads the right side with ' '.
    text.resize(target_size, ' ');

    return text;
}

std::string center_justify(int size, std::string text)
// Centers text within a string of 'size'.
// Returns value at size. 
{
    // 1. Ensure size isn't negative
    std::size_t target_size = (size > 0) ? static_cast<std::size_t>(size) : 0;
    std::size_t text_sz = text.size();

    // 2. Case: Text is too long - Default to left justify/truncate
    if (text_sz >= target_size)
    {
        return left_justify(size, text);
    }

    // 3. Case: Text needs centering
    // Initialize a string of spaces
    std::string return_string(target_size, ' ');

    // Use signed math for the position calculation to be safe, 
    // then cast to unsigned for replace()
    int start_pos = (static_cast<int>(target_size) / 2) - (static_cast<int>(text_sz) / 2);

    // Final safety check: start_pos should not be negative here due to if-check above
    return_string.replace(static_cast<std::size_t>(start_pos), text_sz, text);

    return return_string;
}

 
std::string left_trim(const std::string &Text)
{
    const std::string WHITESPACE = " \n\r\t\f\v";
    std::size_t start = Text.find_first_not_of(WHITESPACE);
    return (start == std::string::npos) ? "" : Text.substr(start);
}

std::string right_trim(const std::string &Text)
{
    const std::string WHITESPACE = " \n\r\t\f\v";
    std::size_t end = Text.find_last_not_of(WHITESPACE);
    // Safety check: if end is npos, substr(0, 0) is correctly an empty string.
    return (end == std::string::npos) ? "" : Text.substr(0, end + 1);
}

std::string trim(const std::string &Text) 
{
    // Calling right_trim(left_trim(Text)) creates an extra temporary string.
    // This combined version is slightly more efficient:
    const std::string WHITESPACE = " \n\r\t\f\v";
    std::size_t start = Text.find_first_not_of(WHITESPACE);
    if (start == std::string::npos) return "";

    std::size_t end = Text.find_last_not_of(WHITESPACE);
    return Text.substr(start, end - start + 1);
}

string lower_case(const std::string &Text) 
{
  string result = Text;
  transform(result.begin(), result.end(), result.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return result;
}

string quotify(string Text)
{
  return "\"" + Text + "\"";
}

//string to_string_hex(I w, size_t hex_len = sizeof(I)<<1) 
string to_string_hex(char Char_Byte) 
{
  static const char* digits = "0123456789ABCDEF";
  
  string ret_number = "";

  int d1 = (Char_Byte % 256 - Char_Byte % 16) / 16;
  int d2 = Char_Byte % 16;
  
  ret_number = digits[d1];
  ret_number = ret_number + digits[d2];

  return ret_number;
}

bool get_bit_value(int baseline, int bit_set_compare)
{
  bool ret_bit_on = false;

  ret_bit_on = (baseline & bit_set_compare) == bit_set_compare;

  return ret_bit_on;
}

string to_string_binary(char Char_Byte)
{
  string ret_number = "";

  ret_number =  to_string(get_bit_value(Char_Byte, 128)) + 
                to_string(get_bit_value(Char_Byte, 64)) + 
                to_string(get_bit_value(Char_Byte, 32)) + 
                to_string(get_bit_value(Char_Byte, 16)) + 
                to_string(get_bit_value(Char_Byte, 8)) + 
                to_string(get_bit_value(Char_Byte, 4)) + 
                to_string(get_bit_value(Char_Byte, 2)) + 
                to_string(get_bit_value(Char_Byte, 1));

  return ret_number;
}

bool left_of_char(std::string Text, char Break_Char, std::string &Left)
{
    bool ret_success = true;

    // Use size_t to match the return type of find()
    std::size_t pos = Text.find(Break_Char);

    if (pos == std::string::npos)
    {
        ret_success = false;
        // Optional: Clear Left so it doesn't contain old data from a previous call
        Left = ""; 
    }
    else
    {
        // No cast needed here because pos is already the correct type
        Left = Text.substr(0, pos);
    }

    return ret_success;
}

bool right_of_char(const std::string &Text, char Break_Char, std::string &Right)
{
    // Use size_t to avoid sign-conversion errors
    std::size_t pos = Text.find(Break_Char);

    if (pos == std::string::npos)
    {
        Right = ""; 
        return false;
    }

    // substr(pos + 1) automatically grabs everything to the end.
    // We check if (pos + 1) is within bounds to be extra safe.
    if (pos + 1 < Text.size())
    {
        Right = Text.substr(pos + 1);
    }
    else
    {
        // Break_Char was the very last character
        Right = "";
    }

    return true;
}

string remove_first_and_last_characters(char Character, string Text)
{
  Text = trim(Text);
  if (Text.size() > 1)
  {
    if (Text[0] == Character)
    {
      if (Text[Text.size() -1] == Character)
      {
        Text = Text.substr(1, Text.size() -2);
      }
    }
  }
  return Text;
}

bool string_hex_to_int(string Hex_String_Value, int &Int_Value)
{
  try
  {
    Int_Value = stoi(Hex_String_Value, 0, 16);
    return true;
  }
  catch(const std::exception& e)
  {
    Int_Value = 0;
    return false;
  }
}

int string_hex_to_int(string Hex_String_Value)
{
  int tmp_int = 0;
  string_hex_to_int(Hex_String_Value, tmp_int);
  return tmp_int;
}

std::string to_string_round_to_nth(float Value, int nth)
{
    // Use to_string to get the initial representation
    std::string ret_string = std::to_string(Value);

    // Use size_t to match string::find's return type
    std::size_t pos = ret_string.find('.');

    if (pos != std::string::npos)
    {
        // Calculate the target length: position of dot + 1 (for dot) + nth digits
        // We use size_t to keep all math unsigned-safe
        std::size_t target_len = pos + static_cast<std::size_t>(nth) + 1;

        if (target_len < ret_string.size())
        {
            // Simply resize the string to the target length
            // This is much safer and cleaner than manual erase math
            ret_string.resize(target_len);
        }
    }
    
    return ret_string;
}

/*
int color_range(float Value, int Magenta, int Red, int Yellow, int Green, int Blue)
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color yellow
// Non zero or mid level green.
{ 
  // 1 - Range Level
  // Magenta  Red  Yellow  Green  Blue  Cyan

  if (Value <= Magenta)
  {
    return COLOR_MAGENTA;
  }
  else if(Value <= Red)
  {
    return COLOR_RED;
  }
  else if(Value <= Yellow)
  {
    return COLOR_YELLOW;
  }
  else if(Value <= Green)
  {
    return COLOR_GREEN;
  }
  else if(Value <= Blue)
  {
    return COLOR_BLUE;
  }
  else
  {
    return COLOR_CYAN;
  }
}

int color_range_reverse(float Value, int Blue, int Green, int Yellow, int Red, int Magenta)
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color yellow
// Non zero or mid level green.
{ 
  // 1 - Range Level
  // Magenta  Red  Yellow  Green  Blue  Cyan

  if (Value <= Blue)
  {
    return COLOR_BLUE;
  }
  else if(Value <= Green)
  {
    return COLOR_GREEN;
  }
  else if(Value <= Yellow)
  {
    return COLOR_YELLOW;
  }
  else if(Value <= Red)
  {
    return COLOR_RED;
  }
  else if(Value <= Magenta)
  {
    return COLOR_MAGENTA;
  }
  else
  {
    return COLOR_CYAN;
  }
}

int color_scale(float Value, int Green, int Yellow, int Red, int Magenta, int Blue)
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color red
// zero level green.
{ 
  // 2 - Scale Level
  // Green  Yellow  Red  Magenta  Blue  Cyan
  
  if (Value <= Green)
  {
    return COLOR_GREEN;
  }
  else if(Value <= Yellow)
  {
    return COLOR_YELLOW;
  }
  else if(Value <= Red)
  {
    return COLOR_RED;
  }
  else if(Value <= Magenta)
  {
    return COLOR_MAGENTA;
  }
  else if(Value <= Blue)
  {
    return COLOR_BLUE;
  }
  else
  {
    return COLOR_CYAN;
  }
}
*/

short xor_checksum(const std::string &Line, char Start_Char, char End_Char)
{
    // Use size_t for indices to match string::find return types
    std::size_t open_pos = Line.find(Start_Char);
    std::size_t close_pos = Line.find_last_of(End_Char);
    short checksum = 0;

    // Check that both characters were actually found
    if (open_pos != std::string::npos && close_pos != std::string::npos)
    {
        // Data starts AFTER the Start_Char
        std::size_t start_index = open_pos + 1;

        // Ensure there is actually content between the two markers
        if (start_index < close_pos)
        {
            for (std::size_t pos = start_index; pos < close_pos; ++pos)
            {
                // Cast to short to maintain type consistency
                checksum ^= static_cast<short>(Line[pos]);
            }
        }
    }

    return checksum;
}

void STRING_STRING::store(string str_value)
{
  STR_VALUE = str_value;
  if(str_value == "")
  {
    CONVERSION_SUCCESS = false;
  }
  else
  {
    CONVERSION_SUCCESS = true;
  }
}

string STRING_STRING::get_str_value()
// Return original number string value.
{
  return STR_VALUE;
}

bool STRING_STRING::conversion_success()
// Returns true if conversion was unsucessful.
{
  return CONVERSION_SUCCESS;
}

void STRING_BOOL::store(string str_value)
{
  STR_VALUE = str_value;

  string eval = lower_case(trim(str_value));

  if (eval == "1" ||
      eval == "true")
  {
    BOOL_VALUE = true;
    CONVERSION_SUCCESS = true;
  }
  else if (eval == "0" ||
            eval == "false")
  {
    BOOL_VALUE = false;
    CONVERSION_SUCCESS = true;
  }
  else
  {
    CONVERSION_SUCCESS = false;
  }
}

string STRING_BOOL::get_str_value()
// Return original number string value.
{
  return STR_VALUE;
}

bool STRING_BOOL::get_bool_value()
// Return converted number string value.
{
  return BOOL_VALUE;
}

bool STRING_BOOL::conversion_success()
// Returns true if conversion was unsucessful.
{
  return CONVERSION_SUCCESS;
}

void STRING_INT::store(string str_value)
{
  STR_VALUE = str_value;

  if (string_to_value(str_value, NEW_INT_VALUE))
  {
    INT_VALUE = NEW_INT_VALUE;
    CONVERSION_SUCCESS = true;
  }
  else
  {
    CONVERSION_SUCCESS = false;
  }
}

string STRING_INT::get_str_value()
// Return original number string value.
{
  return STR_VALUE;
}

int STRING_INT::get_int_value()
// Return converted number string value.
{
  return INT_VALUE;
}

bool STRING_INT::conversion_success()
// Returns true if conversion was unsucessful.
{
  return CONVERSION_SUCCESS;
}


void STRING_FLOAT::store(string str_value)
{
  STR_VALUE = str_value;

  if (string_to_value(str_value, NEW_FLOAT_VALUE))
  {
    FLOAT_VALUE = NEW_FLOAT_VALUE;
    CONVERSION_SUCCESS = true;
  }
  else
  {
    CONVERSION_SUCCESS = false;
  }
}

void STRING_FLOAT::store_val(float value)
{
  FLOAT_VALUE = value;
  STR_VALUE = to_string(value);
  CONVERSION_SUCCESS = true;
}

string STRING_FLOAT::get_str_value()
// Return original number string value.
{
  return STR_VALUE;
}

float STRING_FLOAT::get_float_value()
// Return converted number string value.
{
  return FLOAT_VALUE;
}

int STRING_FLOAT::get_int_value()
// Return converted number string value.
{
  return static_cast<int>(FLOAT_VALUE);
}

bool STRING_FLOAT::conversion_success()
// Returns true if conversion was unsucessful.
{
  return CONVERSION_SUCCESS;
}


void STRING_DOUBLE::store(string str_value)
{
  STR_VALUE = str_value;

  if (string_to_value(str_value, NEW_DOUBLE_VALUE))
  {
    DOUBLE_VALUE = NEW_DOUBLE_VALUE;
    CONVERSION_SUCCESS = true;
  }
  else
  {
    CONVERSION_SUCCESS = false;
  }
}

void STRING_DOUBLE::store_val(double value)
{
  DOUBLE_VALUE = value;
  STR_VALUE = to_string(value);
  CONVERSION_SUCCESS = true;
}

string STRING_DOUBLE::get_str_value()
// Return original number string value.
{
  return STR_VALUE;
}

double STRING_DOUBLE::get_double_value()
// Return converted number string value.
{
  return DOUBLE_VALUE;
}

int STRING_DOUBLE::get_int_value()
// Return converted number string value.
{
  return static_cast<int>(DOUBLE_VALUE);
}

bool STRING_DOUBLE::conversion_success()
// Returns true if conversion was unsucessful.
{
  return CONVERSION_SUCCESS;
}


bool isAlphaNumeric(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

char getRandomAlphaNumeric()
{
  int randomType = rand() % 3; // 0 = digit, 1 = uppercase, 2 = lowercase
  
  if (randomType == 0)
  {
    return static_cast<char>(rand() % 10 + '0');  // 0-9
  }
  else if (randomType == 1)
  {
    return static_cast<char>(rand() % 26 + 'A');  // A-Z
  }
  else
  {
    return static_cast<char>(rand() % 26 + 'a');  // a-z
  }
}


string SEARCH_STRING::value()
{
  if (FOUND)
  {
    return DESTINATION;
  }
  else
  {
    FOUND = true;
    for (size_t pos = 0; pos < DESTINATION.size(); pos++)
    {
      if (SEARCH[pos] != DESTINATION[pos])
      {
        if (!isAlphaNumeric(DESTINATION[pos])) 
        {
          SEARCH[pos] = DESTINATION[pos];
        }
        else
        {
          SEARCH[pos] = getRandomAlphaNumeric();
        }
        FOUND = false;
      }
    }
    return SEARCH;
  }
}

void SEARCH_STRING::set_value(string Value)
{
  if (Value != DESTINATION)
  {
    DESTINATION = Value;
    FOUND = false;
    SEARCH = string(DESTINATION.size(), ' ');
  }
}

string SEARCH_STRING::value(string Value)
{
  set_value(Value);
  return value();
}

string mask_string(string Text, int n)
{
  string ret_string = "";

  for (size_t pos = 0; pos < Text.size(); pos++)
  {
    if (rand() % n == 0 && isAlphaNumeric(Text[pos])) // 25% chance to mask
    {
      ret_string += getRandomAlphaNumeric();
    }
    else
    {
      ret_string += Text[pos];
    }
  }

  return ret_string;
}


#endif