use proc_macro2::TokenStream;
use quote::{format_ident, quote, quote_spanned, ToTokens};
use syn::punctuated::Punctuated;
use syn::Attribute;
use syn::parse::{Parse, ParseStream, Result};
use syn::{parse_macro_input, spanned::Spanned, Data, DeriveInput, Fields,Ident, Index, Token, ItemImpl, ItemStruct, ItemEnum};
use std::collections::HashSet;

fn pymethods(attrs: &HashSet<String>) -> Vec<TokenStream> {
    let mut fns = vec![
        quote! {
            pub fn __str__(&self) -> String {
                format!("{:?}", self)
            }
        },
        quote! {
            #[pyo3(name = "to_json")]
            pub fn _to_json(&self) -> Result<String> {
                <Self as crate::types::PyMethods>::to_json(self)
            }
        },
        quote! {
            #[staticmethod]
            #[pyo3(name = "from_json")]
            pub fn _from_json(s: &str) -> Result<Self> {
                <Self as crate::types::PyMethods>::from_json(s)
            }
        },
    ];
    if !attrs.contains("no_bytes") {
        fns.push(quote! {
            #[pyo3(name = "dump_bytes")]
            pub fn _dump_bytes(&self, args: <Self as crate::types::AsData>::OutArgs) -> Vec<u8> {
                <Self as crate::types::AsData>::dump_bytes::<crate::types::PC>(self, args)
            }
        });
        fns.push(quote! {
            #[staticmethod]
            #[pyo3(name = "from_bytes")]
            pub fn _from_bytes(val: &[u8], args: <Self as crate::types::AsData>::InArgs) -> Result<Self> {
                <Self as crate::types::AsData>::from_bytes::<crate::types::PC>(val, args)
            }
        });
        fns.push(quote! {
            #[pyo3(name = "size_bytes")]
            pub fn _size(&self) -> usize {
                <Self as crate::types::AsData>::size::<crate::types::PC>(self)
            }
        });
    }
    if !attrs.contains("no_new") {
        fns.push(quote! {
            #[new]
            pub fn _default() -> Self {
                Self::default()
            }
        });
    }
    fns
}

struct Args {
    vars: HashSet<String>,
}

impl Parse for Args {
    fn parse(input: ParseStream) -> Result<Self> {
        let vars = Punctuated::<Ident, Token![,]>::parse_terminated(input)?;
        let vars = vars.into_iter().map(|x| x.to_string()).collect();
        Ok(Args { vars })
    }
}

#[proc_macro_attribute]
pub fn basicpymethods(args: proc_macro::TokenStream, item: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let mut args = parse_macro_input!(args as Args);
    if let Ok(mut items) = syn::parse::<ItemImpl>(item.clone()) {
        items.items.extend(pymethods(&args.vars).into_iter().map(|x| syn::parse(x.into()).unwrap()));
        items.into_token_stream().into()
    } else if let Ok(input) = syn::parse::<ItemEnum>(item.clone()) {
        let name = input.ident.clone();
        args.vars.insert("no_new".to_string());
        let fns = TokenStream::from_iter(pymethods(&args.vars));
        quote! {
            #input

            #[pymethods]
            impl #name {
                #fns
            }
        }.into()
    } else if let Ok(input) = syn::parse::<ItemStruct>(item.clone()) {
        let name = input.ident.clone();
        let fns = TokenStream::from_iter(pymethods(&args.vars));
        quote! {
            #input

            #[pymethods]
            impl #name {
                #fns
            }
        }.into()
    } else {
        item
    }
}

#[proc_macro_derive(PyMethods)]
pub fn derive_pymethods(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let name = input.ident;

    quote! {
        impl <'a> crate::types::PyMethods<'a> for #name {
            fn from_json(s: &'a str) -> Result<Self> {
                Ok(serde_json::from_str(s)?)
            }
            fn to_json(&self) -> Result<String> {
                Ok(serde_json::to_string_pretty(&self)?)
            }
        }
    }.into()
}

#[proc_macro_derive(OrderedData, attributes(ordered_data, name_pc, name_xbox, name_ps3))]
pub fn derive_ordered_data_fn(input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let name = input.ident;
    let pc = format_ident!("PC");
    let xbox = format_ident!("XBOX");
    let ps3 = format_ident!("PS3");
    let name_pc = format_ident!("{}PC", name);
    let name_xbox = format_ident!("{}XBOX", name);
    let name_ps3 = format_ident!("{}PS3", name);

    let vis = input.vis;

    let alt_class_pc = alt_class_def(&input.data, &name_pc, &pc);
    let alt_class_xbox = alt_class_def(&input.data, &name_xbox, &xbox);
    let alt_class_ps3 = alt_class_def(&input.data, &name_ps3, &ps3);
    let conv_pc: TokenStream = conv_def(&input.data, &pc);
    let conv_back_pc = conv_back_def(&input.data, &pc);
    let conv_xbox: TokenStream = conv_def(&input.data, &xbox);
    let conv_back_xbox = conv_back_def(&input.data, &xbox);
    let conv_ps3: TokenStream = conv_def(&input.data, &ps3);
    let conv_back_ps3 = conv_back_def(&input.data, &ps3);

    let expanded = quote! {
        #[repr(C)]
        #[derive(Default, Debug, Clone, zerocopy::KnownLayout, zerocopy::FromBytes, zerocopy::IntoBytes, zerocopy::Unaligned, zerocopy::Immutable)]
        #vis #alt_class_pc

        #[repr(C)]
        #[derive(Default, Debug, Clone, zerocopy::KnownLayout, zerocopy::FromBytes, zerocopy::IntoBytes, zerocopy::Unaligned, zerocopy::Immutable)]
        #vis #alt_class_xbox

        #[repr(C)]
        #[derive(Default, Debug, Clone, zerocopy::KnownLayout, zerocopy::FromBytes, zerocopy::IntoBytes, zerocopy::Unaligned, zerocopy::Immutable)]
        #vis #alt_class_ps3


        impl crate::types::OrderedData for #name {
            type PC = #name_pc;
            type XBOX = #name_xbox;
            type PS3 = #name_ps3;
        }

        impl From<#name_pc> for #name {
            fn from(value: #name_pc) -> Self {
                #conv_back_pc
            }
        }

        impl From<#name> for #name_pc {
            fn from(value: #name) -> Self {
                #conv_pc
            }
        }

        impl From<#name_xbox> for #name {
            fn from(value: #name_xbox) -> Self {
                #conv_back_xbox
            }
        }

        impl From<#name> for #name_xbox {
            fn from(value: #name) -> Self {
                #conv_xbox
            }
}

        impl From<#name_ps3> for #name {
            fn from(value: #name_ps3) -> Self {
                #conv_back_ps3
            }
        }

        impl From<#name> for #name_ps3 {
            fn from(value: #name) -> Self {
                #conv_ps3
            }
        }
    };

    proc_macro::TokenStream::from(expanded)
}

fn filter_attrs(attrs: &Vec<Attribute>, endian: &Ident, name: Ident) -> (Ident, bool, Ident) {
    let mut val_endian = endian.clone();
    let mut skip = false;
    for val in attrs.iter().filter(|a| a.path().is_ident("ordered_data")).flat_map(|a| {
        a.parse_args_with(Punctuated::<Ident, Token![,]>::parse_terminated)
            .unwrap()
    }) {
        if val == "PC" || val == "XBOX" || val == "PS3" {
            val_endian = val;
        } else if val == "skipPC" && endian == "PC" {
            skip = true;
        } else if val == "skipXBOX" && endian == "XBOX" {
            skip = true;
        } else if val == "skipPS3" && endian == "PS3" {
            skip = true;
        }
    }
    let alt_name = if endian == "PC" {
        attrs.iter().filter(|a| a.path().is_ident("name_pc")).flat_map(|a| {
            a.parse_args_with(Punctuated::<Ident, Token![,]>::parse_terminated)
                .unwrap()
        }).next()
    } else if endian == "XBOX" {
        attrs.iter().filter(|a| a.path().is_ident("name_xbox")).flat_map(|a| {
            a.parse_args_with(Punctuated::<Ident, Token![,]>::parse_terminated)
                .unwrap()
        }).next()
    } else if endian == "PS3" {
        attrs.iter().filter(|a| a.path().is_ident("name_ps3")).flat_map(|a| {
            a.parse_args_with(Punctuated::<Ident, Token![,]>::parse_terminated)
                .unwrap()
        }).next()
    } else {
        None
    }.unwrap_or(name);

    (val_endian, skip, alt_name)
}

fn conv_def(data: &Data, endian: &Ident) -> TokenStream {
    match *data {
        Data::Struct(ref data) => {
            match data.fields {
                Fields::Named(ref fields) => {
                    let recurse = fields.named.iter().filter_map(|f| {
                        let name = &f.ident;
                        let (_, skip, alt_name) = filter_attrs(&f.attrs, endian, name.clone().unwrap());
                        if !skip {
                            Some(quote_spanned! {
                                f.span() => #name: value.#alt_name.into()
                            })
                        } else {
                            None
                        }
                    });
                    quote! {
                        Self {
                            #(#recurse),*,
                            ..Default::default()
                        }
                    }
                },
                Fields::Unnamed(ref fields) => {
                    let recurse = fields.unnamed.iter().enumerate().map(|(i,f)| {
                        let index = Index::from(i);
                        quote_spanned! {
                            f.span() => value.#index.into()
                        }  
                    });
                    quote! {
                        Self (
                            #(#recurse),*
                        )
                    }
                },
                Fields::Unit => {
                    quote! {
                        Self
                    }
                }
            }
        },
        Data::Enum(_) | Data::Union(_) => unimplemented!(),
    }
}

fn conv_back_def(data: &Data, endian: &Ident) -> TokenStream {
    match *data {
        Data::Struct(ref data) => {
            match data.fields {
                Fields::Named(ref fields) => {
                    let recurse = fields.named.iter().filter_map(|f| {
                        let name = &f.ident;
                        let (_, skip, alt_name) = filter_attrs(&f.attrs, endian, name.clone().unwrap());
                        if !skip {
                            Some(quote_spanned! {
                                f.span() => #alt_name: value.#name.into()
                            })
                        } else {
                            None
                        }
                    });
                    quote! {
                        Self {
                            #(#recurse),*,
                            ..Default::default()
                        }
                    }
                },
                Fields::Unnamed(ref fields) => {
                    let recurse = fields.unnamed.iter().enumerate().map(|(i,f)| {
                        let index = Index::from(i);
                        quote_spanned! {
                            f.span() => value.#index.into()
                        }  
                    });
                    quote! {
                        Self (
                            #(#recurse),*
                        )
                    }
                },
                Fields::Unit => {
                    quote! {
                        Self
                    }
                }
            }
        },
        Data::Enum(_) | Data::Union(_) => unimplemented!(),
    }
}


fn alt_class_def(data: &Data, classname: &Ident, endian: &Ident) -> TokenStream {
    match *data {
        Data::Struct(ref data) => {
            match data.fields {
                Fields::Named(ref fields) => {
                    let recurse = fields.named.iter().filter_map(|f| {
                        let name = &f.ident;
                        let ty = &f.ty;
                        let (val, skip, _) = filter_attrs(&f.attrs, endian, name.clone().unwrap());
                        if !skip {
                            Some(quote_spanned! {
                                f.span() => #name: <#ty as crate::types::OrderedData>::#val
                            })
                        } else {
                            None
                        }
                    });
                    quote! {
                        struct #classname {
                            #(#recurse),*
                        }
                    }
                },
                Fields::Unnamed(ref fields) => {
                    let recurse = fields.unnamed.iter().map(|f| {
                        let ty = &f.ty;
                        quote_spanned! {
                            f.span() => <#ty as crate::types::OrderedData>::#endian
                        }

                    });
                    quote! {
                        struct #classname (
                            #(#recurse),*
                        );
                    }
                },
                Fields::Unit => {
                    quote! {
                        struct #classname;
                    }
                }
            }
        },
        Data::Enum(_) | Data::Union(_) => unimplemented!(),
    }
}
