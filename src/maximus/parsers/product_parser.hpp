#pragma once

#include <functional>
#include <stdexcept>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/optional.hpp>

#include <supermarx/product.hpp>

#include <supermarx/scraper/html_parser.hpp>
#include <supermarx/scraper/html_watcher.hpp>
#include <supermarx/scraper/html_recorder.hpp>
#include <supermarx/scraper/util.hpp>

namespace supermarx
{
	class product_parser : public html_parser::default_handler
	{
	public:
		typedef std::function<void(const product&, boost::optional<std::string>, datetime, confidence, std::vector<std::string>)> product_callback_t;

	private:
		enum state_e {
			S_INIT,
			S_PRODUCT,
			S_PRODUCT_IMAGE,
			S_PRODUCT_BADGE
		};

		product_callback_t product_callback;

		boost::optional<html_recorder> rec;
		html_watcher_collection wc;

		state_e state;

		struct product_proto
		{
			std::string identifier, name;
			boost::optional<std::string> image_uri;
			boost::optional<unsigned int> price;
			boost::optional<std::string> valid_from_to;
			boost::optional<std::string> unit;
			boost::optional<std::string> badge;

			confidence conf = confidence::NEUTRAL;
			std::vector<std::string> problems;
		};

		product_proto current_p;

		static unsigned int parse_price(const std::string& str)
		{
			static const boost::regex match_price("([0-9]*)\\.([0-9]*)");

			boost::smatch what;
			if(!boost::regex_match(str, what, match_price))
				throw std::runtime_error("Could not parse price " + str);

			return boost::lexical_cast<float>(what[1])*100 + boost::lexical_cast<unsigned int>(what[2]);
		}

		static date first_monday(date d)
		{
			while(d.day_of_week() != weekday::Monday)
				d += boost::gregorian::date_duration(1);

			return d;
		}

		void report_problem_understanding(std::string const& field, std::string const& value)
		{
			std::stringstream sstr;
			sstr << "Unclear '" << field << "' with value '" << value << "'";

			current_p.problems.emplace_back(sstr.str());
		}

		void interpret_unit(std::string unit, uint64_t& volume, measure& volume_measure)
		{
			static const boost::regex match_measure("(?:ca. )?([0-9]+(?:\\.[0-9]+)?)(?: )?(g|gr|gram|kg|kilo|ml|cl|lt|liter)(?:\\.)?");
			boost::smatch what;

			if(boost::regex_match(unit, what, match_measure))
			{
				std::string measure_type = what[2];

				if(measure_type == "g" || measure_type == "gr" || measure_type == "gram")
				{
					volume = boost::lexical_cast<float>(what[1])*1000.0;
					volume_measure = measure::MILLIGRAMS;
				}
				else if(measure_type == "kg" || measure_type == "kilo")
				{
					volume = boost::lexical_cast<float>(what[1])*1000000.0;
					volume_measure = measure::MILLIGRAMS;
				}
				else if(measure_type == "ml")
				{
					volume = boost::lexical_cast<float>(what[1]);
					volume_measure = measure::MILLILITERS;
				}
				else if(measure_type == "cl")
				{
					volume = boost::lexical_cast<float>(what[1])*100.0;
					volume_measure = measure::MILLILITERS;
				}
				else if(measure_type == "lt" || measure_type == "liter")
				{
					volume = boost::lexical_cast<float>(what[1])*1000.0;
					volume_measure = measure::MILLILITERS;
				}
				else
				{
					report_problem_understanding("measure_type", measure_type);
					current_p.conf = confidence::LOW;
					return;
				}
			}
			else
			{
				report_problem_understanding("unit", unit);
				current_p.conf = confidence::LOW;
			}
		}

		void interpret_badge(std::string const& badge, uint64_t& price, uint64_t& discount_amount)
		{
			static const boost::regex match_combination_discount("([0-9]+) voor ([0-9]+),([0-9]+)");
			boost::smatch what;

			if(boost::regex_match(badge, what, match_combination_discount))
			{
				discount_amount = boost::lexical_cast<uint64_t>(what[1]);
				price = boost::lexical_cast<float>(what[2] + '.' + what[3])*100.0;
			}
			else
			{
				report_problem_understanding("badge", badge);
				current_p.conf = confidence::LOW;
			}
		}

		void interpret_valid_on(std::string const& valid_from_to, datetime& valid_on)
		{
			static const boost::regex match_valid_from_to("Geldig van ([0-9]+-[0-9]+) t/m [0-9]+-[0-9]+");
			boost::smatch what;

			if(boost::regex_match(valid_from_to, what, match_valid_from_to))
			{
				std::cerr << what[1] << std::endl;
			}
			else
			{
				report_problem_understanding("valid_from_to", valid_from_to);
				current_p.conf = confidence::LOW;
			}
		}

		void deliver_product()
		{
			uint64_t volume = 1;
			measure volume_measure = measure::UNITS;

			if(current_p.unit)
				interpret_unit(*current_p.unit, volume, volume_measure);

			uint64_t orig_price = current_p.price.get();
			uint64_t price = orig_price;
			uint64_t discount_amount = 1;

			if(current_p.badge)
				interpret_badge(*current_p.badge, price, discount_amount);

			datetime valid_on = datetime_now();

			if(current_p.valid_from_to)
				interpret_valid_on(*current_p.valid_from_to, valid_on);

			product_callback(
				product{
					current_p.identifier,
					current_p.name,
					volume,
					volume_measure,
					orig_price,
					price,
					discount_amount,
					datetime_now(), // TODO
				},
				current_p.image_uri,
				valid_on,
				current_p.conf,
				current_p.problems
			);
		}

	public:
		product_parser(product_callback_t product_callback_)
		: product_callback(product_callback_)
		, rec()
		, wc()
		, state(S_INIT)
		, current_p()
		{}

		template<typename T>
		void parse(T source)
		{
			html_parser::parse(source, *this);
		}

		virtual void startElement(const std::string& /* namespaceURI */, const std::string& /* localName */, const std::string& qName, const AttributesT& atts)
		{
			if(rec)
				rec.get().startElement();

			wc.startElement();

			const std::string att_class = atts.getValue("class");

			switch(state)
			{
			case S_INIT:
				if(util::contains_attr("jum-item-product", att_class))
				{
					//Reset product
					current_p = product_proto();
					current_p.identifier = atts.getValue("data-jum-product-sku");

					state = S_PRODUCT;
					wc.add([&]() {
						state = S_INIT;

						deliver_product();
					});
				}
			break;
			case S_PRODUCT:
				if(util::contains_attr("jum-item-titlewrap", att_class))
				{
					rec = html_recorder(
						[&](std::string ch) {
							current_p.name = util::sanitize(ch);
						});
				}
				else if(util::contains_attr("jum-item-figure", att_class))
				{
					state = S_PRODUCT_IMAGE;
					wc.add([&]() {
						state = S_PRODUCT;
					});
				}
				else if(util::contains_attr("jum-price-format", att_class) && !util::contains_attr("jum-comparative-price", att_class))
				{
					// TODO check comparative price / assert

					rec = html_recorder(
						[&](std::string ch) {
							current_p.price = boost::lexical_cast<unsigned int>(util::sanitize(ch));
						});
				}
				else if(util::contains_attr("jum-pack-size", att_class))
				{
					rec = html_recorder(
						[&](std::string ch) {
							current_p.unit = util::sanitize(ch);
						});
				}
				else if(util::contains_attr("jum-promotion-date", att_class))
				{
					rec = html_recorder(
						[&](std::string ch) {
							current_p.valid_from_to = util::sanitize(ch);
						});
				}
				else if(util::contains_attr("jum-item-badges", att_class))
				{
					state = S_PRODUCT_BADGE;
					wc.add([&]() {
						state = S_PRODUCT;
					});
				}
			break;
			case S_PRODUCT_IMAGE:
				if(qName == "img")
				{
					current_p.image_uri = atts.getValue("data-jum-hr-src");
				}
			break;
			case S_PRODUCT_BADGE:
				if(qName == "img")
				{
					current_p.badge = atts.getValue("alt");
				}
			break;
			}
		}

		virtual void characters(const std::string& ch)
		{
			if(rec)
				rec.get().characters(ch);
		}

		virtual void endElement(const std::string& /* namespaceURI */, const std::string& /* localName */, const std::string& /* qName */)
		{
			if(rec && rec.get().endElement())
				rec = boost::none;

			wc.endElement();
		}
	};
}