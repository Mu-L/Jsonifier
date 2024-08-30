/*
	MIT License

	Copyright (c) 2023 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/RealTimeChris/jsonifier
/// Feb 3, 2023
#pragma once

#include <jsonifier/Validator.hpp>
#include <jsonifier/HashMap.hpp>
#include <jsonifier/String.hpp>
#include <jsonifier/Error.hpp>
#include <jsonifier/Simd.hpp>

namespace jsonifier {

	struct parse_options {
		bool validateJson{ false };
		bool minified{ false };
	};

}

namespace jsonifier_internal {

	template<typename derived_type> class parser;

	template<typename derived_type> struct parse_options_internal {
		mutable parser<derived_type>* parserPtr{};
		jsonifier::parse_options optionsReal{};
		mutable int64_t currentObjectDepth{};
		mutable int64_t currentArrayDepth{};
		mutable const char* rootIter{};
		bool validatingJson{ false };
	};

	template<const auto&, typename value_type, typename iterator> struct parse_impl;

	template<typename derived_type> class parser {
	  public:
		template<const auto&, typename value_type, typename iterator> friend struct parse_impl;

		JSONIFIER_ALWAYS_INLINE parser& operator=(const parser& other) = delete;
		JSONIFIER_ALWAYS_INLINE parser(const parser& other)			   = delete;

		template<jsonifier::parse_options options = jsonifier::parse_options{}, typename value_type, jsonifier::concepts::string_t buffer_type>
		JSONIFIER_ALWAYS_INLINE bool parseJson(value_type&& object, buffer_type&& in) noexcept {
			if (in.size() == 0) {
				return false;
			}
			if constexpr (options.validateJson) {
				if (!derivedRef.validateJson(in)) {
					return false;
				}
			}
			static_assert(jsonifier::concepts::printErrorFunction<unwrap_t<value_type>>(), "No specialization of core exists for the type named above - please specialize it!");
			derivedRef.errors.clear();
			static constexpr parse_options_internal<derived_type> optionsReal{ .optionsReal = options };
			optionsReal.parserPtr = this;
			auto readIter		  = static_cast<const char*>(in.data());
			auto endIter		  = static_cast<const char*>(in.data() + in.size());
			optionsReal.rootIter  = readIter;
			if (!readIter || (*readIter != '{' && *readIter != '[')) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::No_Input>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return false;
			}
			parse_impl<optionsReal, value_type, decltype(readIter)>::impl(std::forward<value_type>(object), readIter, endIter);
			if (optionsReal.currentObjectDepth != 0) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::Imbalanced_Object_Braces>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return false;
			} else if (optionsReal.currentArrayDepth != 0) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::Imbalanced_Array_Brackets>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return false;
			}
			return true;
		}

		template<typename value_type, jsonifier::parse_options options = jsonifier::parse_options{}, jsonifier::concepts::string_t buffer_type>
		JSONIFIER_ALWAYS_INLINE value_type parseJson(buffer_type&& in) noexcept {
			if (in.size() == 0) {
				return value_type{};
			}
			if constexpr (options.validateJson) {
				if (!derivedRef.validateJson(in)) {
					return value_type{};
				}
			}
			static_assert(jsonifier::concepts::printErrorFunction<unwrap_t<value_type>>(), "No specialization of core exists for the type named above - please specialize it!");
			derivedRef.errors.clear();
			static constexpr parse_options_internal<derived_type> optionsReal{ .optionsReal = options };
			optionsReal.parserPtr = this;
			optionsReal.rootIter  = static_cast<const char*>(in.data());
			unwrap_t<value_type> object{};
			auto readIter = static_cast<const char*>(in.data());
			auto endIter  = static_cast<const char*>(in.data() + in.size());
			if (!readIter || (*readIter != '{' && *readIter != '[')) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::No_Input>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return object;
			}
			parse_impl<optionsReal, value_type, decltype(readIter)>::impl(std::forward<value_type>(object), readIter, endIter);
			if (optionsReal.currentObjectDepth != 0) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::Imbalanced_Object_Braces>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return false;
			} else if (optionsReal.currentArrayDepth != 0) {
				static constexpr auto sourceLocation{ std::source_location::current() };
				getErrors().emplace_back(error::constructError<sourceLocation, error_classes::Parsing, parse_errors::Imbalanced_Array_Brackets>(readIter - optionsReal.rootIter,
					endIter - optionsReal.rootIter, optionsReal.rootIter));
				return false;
			}
			return object;
		}

		JSONIFIER_ALWAYS_INLINE jsonifier::vector<error>& getErrors() noexcept {
			return derivedRef.errors;
		}

	  protected:
		derived_type& derivedRef{ initializeSelfRef() };

		JSONIFIER_ALWAYS_INLINE parser() noexcept : derivedRef{ initializeSelfRef() } {};

		JSONIFIER_ALWAYS_INLINE derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		JSONIFIER_ALWAYS_INLINE ~parser() noexcept = default;
	};
};