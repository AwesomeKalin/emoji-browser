// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/content/renderer/page_passwords_analyser.h"

#include <stack>

#include "base/lazy_instance.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "components/autofill/content/renderer/form_autofill_util.h"
#include "components/autofill/content/renderer/page_form_analyser_logger.h"
#include "components/autofill/content/renderer/password_form_conversion_utils.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_element_collection.h"
#include "third_party/blink/public/web/web_form_control_element.h"
#include "third_party/blink/public/web/web_label_element.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/re2/src/re2/re2.h"

namespace autofill {

namespace {

const char kDocumentationUrl[] = "https://goo.gl/9p2vKq";
const char* kTypeAttributes[] = {"text", "email", "tel", "password"};
const char* kTypeTextAttributes[] = {"text", "email", "tel"};
char kTextFieldSignature = 'T';
char kPasswordFieldSignature = 'P';

// Produce a relevant link to developer documentation regarding the warning or
// error. If no particular reference is given, the default URL will be provided.
// Otherwise, the URL will point to the specified anchor.
std::string LinkDocumentation(const std::string& message,
                              const char* reference = nullptr) {
  std::string documented = message + " (More info: " + kDocumentationUrl + ")";
  if (reference)
    return documented + std::string("#") + reference;
  return documented;
}

// A simple wrapper that provides some extra data about nodes
// during the DOM traversal (e.g. whether it lies within a <form>
// element, which is necessary for some of the warnings).
struct TraversalInfo {
  const blink::WebNode node;
  const bool in_form;
};

// Collects the important elements in a form that are
// relevant to the Password Manager, which consists of the text and password
// inputs in a form, as well as their ordering.
struct FormInputCollection {
  blink::WebFormElement form;
  std::vector<blink::WebFormControlElement> inputs;
  std::vector<size_t> text_inputs;
  std::vector<size_t> password_inputs;
  std::vector<size_t> explicit_password_inputs;
  std::string signature;

  // The signature of a form is a string of 'T's and 'P's, representing
  // username and password fields respectively. This is used to quickly match
  // against well-known <input> patterns to guess what kind of form we are
  // dealing with, and provide intelligent autocomplete suggestions.
  void AddInput(const blink::WebFormControlElement& input) {
    std::string type(
        input.HasAttribute("type") ? input.GetAttribute("type").Utf8() : "");
    signature +=
        type != "password" ? kTextFieldSignature : kPasswordFieldSignature;
    if (type != "password") {
      text_inputs.push_back(inputs.size());
    } else {
      password_inputs.push_back(inputs.size());
      if (input.HasAttribute("autocomplete")) {
        // There are some warnings we only throw if we are certain that a
        // password field is actually a password (rather than a credit card
        // security code, etc.).
        std::string autocomplete(input.GetAttribute("autocomplete").Utf8());
        if (autocomplete == "current-password" ||
            autocomplete == "new-password")
          explicit_password_inputs.push_back(inputs.size());
      }
    }
    inputs.push_back(input);
  }
};

#define DECLARE_LAZY_MATCHER(NAME, PATTERN)                                   \
  struct LabelPatternLazyInstanceTraits_##NAME                                \
      : public base::internal::DestructorAtExitLazyInstanceTraits<re2::RE2> { \
    static re2::RE2* New(void* instance) {                                    \
      return CreateMatcher(instance, PATTERN);                                \
    }                                                                         \
  };                                                                          \
  base::LazyInstance<re2::RE2, LabelPatternLazyInstanceTraits_##NAME> NAME =  \
      LAZY_INSTANCE_INITIALIZER

DECLARE_LAZY_MATCHER(ignored_characters_matcher, R"(\W)");
DECLARE_LAZY_MATCHER(username_matcher, R"(user(name)?|login)");
DECLARE_LAZY_MATCHER(email_matcher, R"(email(address)?)");
DECLARE_LAZY_MATCHER(telephone_matcher, R"((mobile)?(telephone)?(number|no))");

#undef DECLARE_LAZY_MATCHER

// Represents a common <label> content text-pattern that indicates
// something of the purpose of an element (for example: that it is a username
// field).
struct InputHint {
  const re2::RE2* regex;
  size_t match;

  InputHint(const re2::RE2* regex) : regex(regex), match(std::string::npos) {}

  void MatchLabel(std::string& label_content, size_t index) {
    if (re2::RE2::FullMatch(label_content, *regex))
      match = index;
  }
};

// Multiple semantic forms may be contained within a single <form> element,
// which causes confusion to the Password Manager, which acts under the
// assumption each <form> element corresponds to a single form.
// |FormIsTooComplex| uses a simple heuristic to guess whether a form may
// contain too many inputs to be considered a single form.
bool FormIsTooComplex(const std::string& signature) {
  unsigned kind_changes = 0;
  unsigned password_count = 0;
  for (const char kind : signature) {
    if (kind ==
        (kind_changes & 1 ? kTextFieldSignature : kPasswordFieldSignature))
      ++kind_changes;
    password_count += kind == kPasswordFieldSignature;
  }
  return kind_changes >= 3 || password_count > 3;
}

// Stores an element's id in |ids| for duplicity-checking.
void TrackElementId(
    const blink::WebElement& element,
    std::map<std::string, std::vector<blink::WebNode>>* nodes_for_id) {
  if (element.HasAttribute("id")) {
    std::string id_attr = element.GetAttribute("id").Utf8();
    (*nodes_for_id)[id_attr].push_back(element);
  }
}

// We don't want to re-analyse the same nodes each time the method is
// called. This technically means some warnings might be overlooked (for
// example if an invalid attribute is added), but these cases are assumed
// to be rare, and are ignored for the sake of simplicity.
// The id of |node| will additionally be added to the corresponding |ids| set.
bool TrackElementIfUntracked(
    const blink::WebElement& node,
    std::set<blink::WebNode>* skip_nodes,
    std::map<std::string, std::vector<blink::WebNode>>* nodes_for_id) {
  if (skip_nodes->count(node))
    return true;
  skip_nodes->insert(node);
  // If we don't skip the node, we want to make sure its id is tracked.
  TrackElementId(node, nodes_for_id);
  return false;
}

// Error and warning messages regarding the DOM structure: missing <form> tags,
// duplicate ids, etc. Returns a list of the forms found in the DOM for further
// analysis.
std::vector<FormInputCollection> ExtractFormsForAnalysis(
    const blink::WebDocument& document,
    std::set<blink::WebNode>* skip_nodes,
    PageFormAnalyserLogger* logger) {
  std::vector<FormInputCollection> form_input_collections;

  // Keep track of inputs that are inside <form> elements to find the complement
  // for warnings afterwards.
  std::set<blink::WebFormControlElement> inputs_with_forms;
  std::map<std::string, std::vector<blink::WebNode>> nodes_for_id;

  blink::WebVector<blink::WebFormElement> forms;
  document.Forms(forms);
  for (const blink::WebFormElement& form : forms) {
    form_input_collections.push_back(FormInputCollection{form});
    // Collect all the inputs in the form.
    blink::WebVector<blink::WebFormControlElement> form_control_elements;
    form.GetFormControlElements(form_control_elements);
    for (const blink::WebFormControlElement& input : form_control_elements) {
      if (TrackElementIfUntracked(input, skip_nodes, &nodes_for_id))
        continue;
      // We are only interested in a subset of input elements -- those likely
      // to be username or password fields.
      if (input.TagName() == "INPUT" &&
          (!input.HasAttribute("type") ||
           base::Contains(kTypeAttributes,
                          input.GetAttribute("type").Utf8()))) {
        form_input_collections.back().AddInput(input);
        inputs_with_forms.insert(input);
      }
    }

    TrackElementIfUntracked(form, skip_nodes, &nodes_for_id);
  }

  // Check for password fields that are not contained inside forms.
  auto password_inputs = document.QuerySelectorAll("input[type=\"password\"]");
  for (unsigned i = 0; i < password_inputs.size(); ++i) {
    if (TrackElementIfUntracked(password_inputs[i], skip_nodes, &nodes_for_id))
      continue;
    // Any password fields inside <form> elements will have been skipped,
    // leaving just those without associated forms.
    logger->Send(
        LinkDocumentation("Password field is not contained in a form:"),
        PageFormAnalyserLogger::kVerbose, password_inputs[i]);
  }
  // Check for input fields that are not contained inside forms, to make sure
  // their id attributes don't conflict with other fields also not contained
  // inside forms.
  std::string selector = "input:not([type])";
  for (const char* text_type : kTypeTextAttributes)
    selector += ", input[type=\"" + std::string(text_type) + "\"]";
  auto text_inputs =
      document.QuerySelectorAll(blink::WebString::FromUTF8(selector));
  for (const blink::WebElement& text_input : text_inputs)
    TrackElementIfUntracked(text_input, skip_nodes, &nodes_for_id);

  // Warn against elements sharing an id attribute. Duplicate id attributes both
  // are against the HTML specification and can cause issues with password
  // saving/filling, as the Password Manager makes the assumption that ids may
  // be used as a unique identifier for nodes.
  for (const auto& pair : nodes_for_id) {
    const std::string& id_attr = pair.first;
    const std::vector<blink::WebNode>& nodes = pair.second;
    if (nodes.size() <= 1)
      continue;
    if (!id_attr.empty()) {
      logger->Send(LinkDocumentation(base::StringPrintf(
                       "Found %zu elements with non-unique id #%s:",
                       nodes.size(), id_attr.c_str())),
                   PageFormAnalyserLogger::kWarning, nodes);
    } else {
      logger->Send(LinkDocumentation(base::StringPrintf(
                       "Found %zu elements with non-unique id #%s:",
                       nodes.size(), id_attr.c_str())),
                   PageFormAnalyserLogger::kWarning, nodes);
    }
  }

  return form_input_collections;
}

// The username field is the most difficult field to identify, as there
// are often many other textual fields in a form, and it is not always
// possible to work out which one is the username. Here, we find any
// <label> elements pointing to the input fields, and check their content.
// Labels containing text such as "Username:" or "Email address:" are
// likely to indicate the desired field, and will be prioritised over
// other fields.
void InferUsernameField(
    const blink::WebFormElement& form,
    const std::vector<blink::WebFormControlElement>& inputs,
    size_t username_field_guess,
    std::map<size_t, std::string>* autocomplete_suggestions) {
  blink::WebElementCollection labels(form.GetElementsByHTMLTagName("label"));
  DCHECK(!labels.IsNull());

  std::vector<InputHint> input_hints;

  input_hints.push_back(InputHint(username_matcher.Pointer()));
  input_hints.push_back(InputHint(email_matcher.Pointer()));
  input_hints.push_back(InputHint(telephone_matcher.Pointer()));

  for (blink::WebElement item = labels.FirstItem(); !item.IsNull();
       item = labels.NextItem()) {
    blink::WebLabelElement label(item.To<blink::WebLabelElement>());
    blink::WebElement control(label.CorrespondingControl());
    if (!control.IsNull() && control.IsFormControlElement()) {
      blink::WebFormControlElement form_control(
          control.To<blink::WebFormControlElement>());
      auto found = std::find(inputs.begin(), inputs.end(), form_control);
      if (found != inputs.end()) {
        std::string label_content(
            base::UTF16ToUTF8(form_util::FindChildText(label)));
        // Reduce to plain-text, as labels often contain extra punctuation.
        re2::RE2::GlobalReplace(&label_content,
                                ignored_characters_matcher.Get(), "");
        for (InputHint& input_hint : input_hints)
          input_hint.MatchLabel(label_content, found - inputs.begin());
      }
    }
  }

  for (InputHint& input_hint : input_hints) {
    if (input_hint.match != std::string::npos) {
      username_field_guess = input_hint.match;
      break;
    }
  }

  (*autocomplete_suggestions)[username_field_guess] = "username";
}

// Infer what kind of form a form corresponds to (e.g. a
// registration, log-in or password reset form), based on the structure of
// the form.
void GuessAutocompleteAttributesForPasswordFields(
    const std::vector<size_t>& password_inputs,
    bool has_text_field,
    std::map<size_t, std::string>* autocomplete_suggestions) {
  size_t password_count = password_inputs.size();
  switch (password_count) {
    case 3:
      (*autocomplete_suggestions)[password_inputs[0]] = "current-password";
      FALLTHROUGH;  // To match the last two password fields.
    case 2:
      (*autocomplete_suggestions)[password_inputs[password_count - 2]] =
          "new-password";
      (*autocomplete_suggestions)[password_inputs[password_count - 1]] =
          "new-password";
      break;
    case 1:
      (*autocomplete_suggestions)[password_inputs[password_count - 1]] =
          has_text_field ? "current-password" : "new-password";
      break;
  }
}

// Error and warning messages specific to an individual form (for example,
// autocomplete attributes, or missing username fields, etc.).
void AnalyseForm(const FormInputCollection& form_input_collection,
                 PageFormAnalyserLogger* logger) {
  const blink::WebFormElement& form = form_input_collection.form;
  const std::vector<blink::WebFormControlElement>& inputs =
      form_input_collection.inputs;
  const std::vector<size_t>& text_inputs = form_input_collection.text_inputs;
  const std::vector<size_t>& explicit_password_inputs =
      form_input_collection.explicit_password_inputs;
  const std::vector<size_t>& password_inputs =
      form_input_collection.password_inputs;
  const std::string& signature = form_input_collection.signature;

  // We're only interested in forms that contain password fields.
  if (password_inputs.empty())
    return;

  bool has_text_field = !text_inputs.empty();
  size_t username_field_guess =
      0;  // Give it a default value to keep the compiler happy.

  // In order to decrease number of messages and chance of false positives show
  // username suggestions only when password fields are annotated.
  if (!explicit_password_inputs.empty()) {
    if (!has_text_field || text_inputs[0] > explicit_password_inputs[0]) {
      // There is no formal requirement to have associated username fields for
      // every password field, but providing one ensures that the Password
      // Manager associates the correct account name with the password (for
      // example in password reset forms).
      logger->Send(
          LinkDocumentation("Password forms should have (optionally hidden) "
                            "username fields for accessibility:"),
          PageFormAnalyserLogger::kVerbose, form);
    } else {
      // By default (if the other heuristics fail), the first text field
      // preceding a password field will be considered the username field.
      for (username_field_guess = explicit_password_inputs[0] - 1;;
           --username_field_guess) {
        DCHECK(username_field_guess < signature.size());
        if (signature[username_field_guess] == kTextFieldSignature)
          break;
      }
    }
  }

  if (FormIsTooComplex(signature)) {
    logger->Send(
        LinkDocumentation(
            "Multiple forms should be contained in their own "
            "form elements; break up complex forms into ones that represent a "
            "single action:"),
        PageFormAnalyserLogger::kVerbose, form);
    return;
  }

  // The autocomplete attribute provides valuable hints to the Password
  // Manager as to the semantic structure of a form. Rather than simply point
  // out that an autocomplete attribute would be useful, we try to suggest the
  // intended value of the autocomplete attribute in order to save time for
  // the developer.
  std::map<size_t, std::string> autocomplete_suggestions;
  // If there are no password fields that have been explicitly declared
  // passwords, we don't suggest an autocomplete="username" attribute, to stop
  // false positives associated with credit card details.
  if (!explicit_password_inputs.empty() && has_text_field &&
      text_inputs[0] < explicit_password_inputs[0]) {
    InferUsernameField(form, inputs, username_field_guess,
                       &autocomplete_suggestions);
  }

  GuessAutocompleteAttributesForPasswordFields(password_inputs, has_text_field,
                                               &autocomplete_suggestions);

  // For each input element that is not annotated with an autocomplete
  // attribute, if we have a guess for what function the input serves, log
  // a warning, suggesting that the inferred attribute value should be added.
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (autocomplete_suggestions.count(i) &&
        !inputs[i].HasAttribute("autocomplete"))
      logger->Send(LinkDocumentation("Input elements should have autocomplete "
                                     "attributes (suggested: \"" +
                                     autocomplete_suggestions[i] + "\"):"),
                   PageFormAnalyserLogger::kVerbose, inputs[i]);
  }
}

}  // namespace

// Out-of-line definitions to keep [chromium-style] happy.
PagePasswordsAnalyser::PagePasswordsAnalyser() {}

PagePasswordsAnalyser::~PagePasswordsAnalyser() {}

void PagePasswordsAnalyser::Reset() {
  skip_nodes_.clear();
}

void PagePasswordsAnalyser::AnalyseDocumentDOM(blink::WebLocalFrame* frame,
                                               PageFormAnalyserLogger* logger) {
  DCHECK(frame);

  blink::WebDocument document(frame->GetDocument());
  // Extract all the forms from the DOM, and provide relevant warnings.
  std::vector<FormInputCollection> forms(
      ExtractFormsForAnalysis(document, &skip_nodes_, logger));

  // Analyse each form in turn, for example with respect to autocomplete
  // attributes.
  for (const FormInputCollection& form_input_collection : forms)
    AnalyseForm(form_input_collection, logger);

  // Finally, send all the warnings and errors to the console.
  logger->Flush();
}

void PagePasswordsAnalyser::AnalyseDocumentDOM(blink::WebLocalFrame* frame) {
  PageFormAnalyserLogger logger(frame);
  AnalyseDocumentDOM(frame, &logger);
}

}  // namespace autofill
