
// ***************************************************************************************
// *
// *    Core       | Everything within this document is proprietary to Core Dynamics.
// *    Dynamics   | Any unauthorized duplication will be subject to prosecution.
// *
// *    Department : (R+D)^2                        Name: stringthings.h
// *       Sub Dept: Programming
// *    Location ID: 856-45B
// *                                                      (c) 2856 - 2858 Core Dynamics
// ***************************************************************************************

#ifndef STRINGTHINGS_H
#define STRINGTHINGS_H

// Standard Header Files
#include <iostream>
#include <string>
#include <deque>
#include <algorithm>
#include <cctype>


using namespace std;

int count_char_in_string(string& Text, char Character);

string char_buf_to_string(char* Buf, int Buf_Len);

string filter_non_printable(const string& input);
// Filter out non printable characters from string by replacing 
//  them with '.'.

string filter_all_non_printable(const string& input);
// Filter out non printable characters from string by replacing 
//  them with '.'.

string line_create(int Size, char Character);
// Returns an character filled line of Size.

bool starts_with(const std::string& s, const std::string& prefix);

string linefill(int size, string text);
// is broken
// Returns a space filled line of size with text in center.

string linemerge_left_justify(int size, string line, string text);
// Overlaps and right justifies text onto line.
// Returns value at size of size. 

string linemerge_left_justify(string line, string text);
// Overlaps and left justifies text onto line.
// No Truncate.
// Returns value at size of line or text size, whichever
//  is larger.

string linemerge_right_justify(int size, string line, string text);
// Overlaps and right justifies text onto line.
// Returns value at size. 

string right_justify(int size, string text);
// Overlaps and right justifies text onto line.
// Returns value at size. 

string left_justify(int size, string text);
// Overlaps and left justifies text onto line.
// Returns value at size. 

string center_justify(int size, string text);
// Overlaps and left justifies text onto line.
// Returns value at size. 

string left_trim(const string &Text);
// Remove leading spaces and special characters from text.

string right_trim(const string &Text);
// Remove trailing spaces and special characters from text.

string trim(const string &Text);
// Remove leading and trailing spaces and special characters from text.

string lower_case(const string &Text);
// converts text to lower case.

string quotify(string Text);
// Put quotes around string;

string to_string_hex(char Char_Byte);
// Returns string two byte hex value from byte.

bool get_bit_value(int baseline, int bit_set_compare);

string to_string_binary(char Char_Byte);
// Returns 8 bit binary value from byte.

bool left_of_char(string Text, char Break_Char, string &Left);
// Returns left of Break_Char as Left;
// Returns true if Break_Char exist.

bool right_of_char(string Text, char Break_Char, string &Right);
// Returns right of Break_Char as Right;
// Returns true if Break_Char exist.

string remove_first_and_last_characters(char Character, string Text);
// For removing things like start and ending quotes.
// Value will be trimmed.

bool string_hex_to_int(string String_Value, int &Int_Value);
// Convert in String_Value number to out Int_Value.
//  Returns true if value sucessful.

int string_hex_to_int(string String_Value);
// Convert in String_Value number to out int.
//  Returns Integer Value

string to_string_round_to_nth(float Value, int nth);
// Returns string of float rounded to nth decimal.
//  like: float_to_string

//int color_range(float Value, int Magenta, int Red, int Yellow, int Green, int Blue);
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color yellow
// Non zero or mid level green.

//int color_range_reverse(float Value, int Blue, int Green, int Yellow, int Red, int Magenta);
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color yellow
// Non zero or mid level green.

//int color_scale(float Value, int Green, int Yellow, int Red, int Magenta, int Blue);
// Returns color in ranges of 1st to 5th of values
// eg (12, 5, 10, 15, 20, 25) returns color red
// zero level green.

short xor_checksum(string Line, char Start_Char, char End_Char);
// Short CHECKSUM

/**
 * @brief Checks if a type is one of the supported numeric types.
 *
 * This helper template is used by the static_asserts to validate the types.
 * @tparam T The type to check.
 */

template <typename T>
constexpr bool is_supported_type_v =
    std::is_same_v<T, int> ||
    std::is_same_v<T, unsigned int> ||
    std::is_same_v<T, long> ||
    std::is_same_v<T, unsigned long> ||
    std::is_same_v<T, long long> ||
    std::is_same_v<T, unsigned long long> ||
    std::is_same_v<T, float> ||
    std::is_same_v<T, double>;

/**
 * @brief Converts a string to a specified numeric type, returning the value directly.
 *
 * @tparam T The target numeric type.
 * @param String_Value The string to convert.
 * @return The converted numeric value of type T.
 */
template <typename T>
T string_to_value(const std::string& String_Value)
/*
  MUST BE ASSIGNE AS:
    string_to_value<int>(string)
    string_to_value<float>(string)
    string_to_value<ulong>(string)
    string_to_value<double>(string)
*/
{
  static_assert(is_supported_type_v<T>,
                "The string_to_value function can only be used with specific integer or floating-point types.");

  static_assert(
      !std::is_unsigned<T>::value || std::is_integral<T>::value,
      "Unsigned floating-point types (e.g., unsigned float, unsigned double) are not standard C++ types."
  );

  try
  {
    if constexpr (std::is_same_v<T, int>)
    {
      return std::stoi(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned int>)
    {
      return std::stoul(String_Value);
    }
    else if constexpr (std::is_same_v<T, long>)
    {
      return std::stol(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned long>)
    {
      return std::stoul(String_Value);
    }
    else if constexpr (std::is_same_v<T, long long>)
    {
      return std::stoll(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned long long>)
    {
      return std::stoull(String_Value);
    }
    else if constexpr (std::is_same_v<T, float>)
    {
      return std::stof(String_Value);
    }
    else // This must be T = double.
    {
      return std::stod(String_Value);
    }
  }
  catch (const std::exception& e)
  {
    std::cerr << "Conversion error: " << e.what() << " String: " << String_Value << std::endl;
    return T{};
  }
}

/**
 * @brief Converts a string to a specified numeric type, returning a boolean for success.
 *
 * @tparam T The target numeric type.
 * @param String_Value The string to convert.
 * @param Result_Value A reference to the variable to store the result.
 * @return true if the conversion was successful, false otherwise.
 */
template <typename T>
bool string_to_value(const std::string& String_Value, T& Result_Value)
{
  static_assert(is_supported_type_v<T>,
    "The string_to_value function can only be used with specific integer or floating-point types.");

  static_assert(
    !std::is_unsigned<T>::value || std::is_integral<T>::value,
    "Unsigned floating-point types (e.g., unsigned float, unsigned double) are not standard C++ types."
  );

  try
  {
    if constexpr (std::is_same_v<T, int>)
    {
      Result_Value = std::stoi(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned int>)
    {
      Result_Value = std::stoul(String_Value);
    }
    else if constexpr (std::is_same_v<T, long>)
    {
      Result_Value = std::stol(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned long>)
    {
      Result_Value = std::stoul(String_Value);
    }
    else if constexpr (std::is_same_v<T, long long>)
    {
      Result_Value = std::stoll(String_Value);
    }
    else if constexpr (std::is_same_v<T, unsigned long long>)
    {
      Result_Value = std::stoull(String_Value);
    }
    else if constexpr (std::is_same_v<T, float>)
    {
      Result_Value = std::stof(String_Value);
    }
    else // This must be T = double.
    {
      Result_Value = std::stod(String_Value);
    }
    return true;
  }
  catch (const std::exception& e)
  {
    // error reported off because true returns false on error
    //std::cerr << "Conversion error: " << e.what() << " String: " << String_Value << std::endl;
    Result_Value = T{}; // Ensure the value is reset on failure
    return false;
  }
}

class STRING_STRING
// Variable stores original string number and converted value.
// Sets error if conversion unsucessful.
{
  private:
  string  STR_VALUE = "";
  bool    CONVERSION_SUCCESS = false;

  public:

  void store(string str_value);

  string get_str_value();

  bool conversion_success();

};

class STRING_BOOL
// Variable stores original string number and converted value.
// Sets error if conversion unsucessful.
// Retains value if conversion not sucessful.
// Sets to true if string == true or 1;
{
  private:
  string  STR_VALUE = "";
  bool    BOOL_VALUE = false;
  bool    CONVERSION_SUCCESS = false;

  public:
  void store(string str_value);

  string get_str_value();

  bool get_bool_value();

  bool conversion_success();

};

class STRING_INT
// Variable stores original string number and converted value.
// Sets error if conversion unsucessful.
// Retains value if conversion not sucessful.
{
  private:
  string  STR_VALUE = "";
  int     INT_VALUE = 0;
  int     NEW_INT_VALUE = 0;
  bool    CONVERSION_SUCCESS = false;

  public:
  void store(string str_value);

  string get_str_value();

  int get_int_value();

  bool conversion_success();

};

class STRING_FLOAT
// Variable stores original string number and converted value.
// Sets error if conversion unsucessful.
// Retains value if conversion not sucessful.
{
  private:
  string  STR_VALUE = "";
  float   FLOAT_VALUE = 0.0f;
  float   NEW_FLOAT_VALUE = 0.0f;
  bool    CONVERSION_SUCCESS = false;

  public:
  void store(string str_value);

  void store_val(float value);
  
  string get_str_value();

  float get_float_value();

  int get_int_value();

  bool conversion_success();
};

class STRING_DOUBLE
// Variable stores original string number and converted value.
// Sets error if conversion unsucessful.
// Retains value if conversion not sucessful.
{
  private:
  string  STR_VALUE = "";
  double   DOUBLE_VALUE = 0.0;
  double   NEW_DOUBLE_VALUE = 0.0;
  bool    CONVERSION_SUCCESS = false;

  public:
  void store(string str_value);

  void store_val(double value);
  
  string get_str_value();

  double get_double_value();

  int get_int_value();

  bool conversion_success();
};

bool isAlphaNumeric(char c);
char getRandomAlphaNumeric();

class SEARCH_STRING
// Slowly roles in string from initial random charters.
{
  private: 
  string DESTINATION = "";
  string SEARCH = "";
  bool FOUND = false;

  public:
  string value();
  void set_value(string Value);
  string value(string Value);
};

string mask_string(string Text, int n);
// Randomizes every nth character in Text.
//  Only works with screen shots.  Will not thwart 
//  off a camera.


#endif
